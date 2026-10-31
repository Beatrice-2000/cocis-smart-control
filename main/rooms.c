/*
 * LECTURE ROOM CONTROL (runs as 2 identical groups of objects inside the one ESP32)
 *
 *   PIR interrupt --> restart one-shot "vacancy" timer + wake the room task
 *   vacancy timer expires (no motion for OCC_HOLD_MS) --> room is vacant
 *   room task: lamp = occupied AND dark AND building open   (unless manual mode)
 *
 * The room tasks READ the building state straight from the access module
 * (sched_is_open(), access_occupancy()). Those functions take their own mutexes,
 * so reading them from another task is safe.
 */
#include <stdio.h>
#include <string.h>
#include "app_config.h"
#include "rooms.h"
#include "access.h"
#include "store.h"
#include "door.h"
#include "timesync.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/semphr.h"

static const char *TAG = "rooms";
typedef enum { LM_AUTO = 0, LM_ON, LM_OFF } light_mode_t;
static const char *MODE_NAME[] = { "auto", "on", "off" };

typedef struct {
    int pir_pin, lamp_pin; adc_channel_t ldr;
    volatile bool occupied;                 /* written by the ISR and the timer callback */
    light_mode_t mode; bool lamp; int raw;
    uint32_t on_s, up_s;
    SemaphoreHandle_t lock;                 /* protects everything above except 'occupied' */
    TimerHandle_t vacancy;
    TaskHandle_t task;
} room_t;

static room_t s_room[NUM_ROOMS];
static adc_oneshot_unit_handle_t s_adc;

static void IRAM_ATTR pir_isr(void *arg)
{
    room_t *r = (room_t *)arg;
    BaseType_t woken = pdFALSE;
    r->occupied = true;                                     /* motion = occupied          */
    xTimerResetFromISR(r->vacancy, &woken);                 /* restart the "no motion" countdown */
    vTaskNotifyGiveFromISR(r->task, &woken);                /* wake this room's task      */
    if (woken) portYIELD_FROM_ISR();
}
static void vacancy_cb(TimerHandle_t t)
{
    room_t *r = (room_t *)pvTimerGetTimerID(t);
    r->occupied = false;
    xTaskNotifyGive(r->task);
}

static int read_ldr(room_t *r)
{
    int sum = 0, v = 0;
    for (int i = 0; i < 4; i++) { adc_oneshot_read(s_adc, r->ldr, &v); sum += v; }
    return sum / 4;
}
static bool is_dark(int raw, bool lamp_on)
{
    int margin = lamp_on ? 300 : 0;                         /* hysteresis: avoids flicker at the threshold */
#if LDR_BRIGHT_IS_HIGH
    return raw < DARK_BELOW_RAW + margin;
#else
    return raw > DARK_BELOW_RAW - margin;
#endif
}

static void room_task(void *arg)
{
    room_t *r = (room_t *)arg;
    int id = (int)(r - s_room) + 1;
    int64_t last_tick = esp_timer_get_time(), last_alert = 0;
    bool prev_lamp = false, prev_occ = false; light_mode_t prev_mode = LM_AUTO;

    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));       /* wake on motion/event or every 0.5 s */
        int raw = read_ldr(r);
        int64_t now = esp_timer_get_time();

        /* building state from the access module (own mutexes, taken BEFORE our own lock) */
        bool clock_ok = time_valid();
        bool open = sched_is_open();
        int  inside = access_occupancy();
        bool closed = clock_ok && !open;

        const char *alert = NULL;
        xSemaphoreTake(r->lock, portMAX_DELAY);
        r->raw = raw;
        bool occ = r->occupied, want;
        if (r->mode == LM_ON)       want = true;
        else if (r->mode == LM_OFF) want = false;
        else                        want = occ && !closed && is_dark(raw, r->lamp);
        r->lamp = want;
        gpio_set_level(r->lamp_pin, want);

        while (now - last_tick >= 1000000) { last_tick += 1000000; r->up_s++; if (r->lamp) r->on_s++; }

        if (occ && clock_ok && r->mode == LM_AUTO) {        /* security cross-check */
            if (closed) alert = "motion while building closed";
            else if (inside == 0) alert = "presence without anybody scanned in";
        }
        if (r->lamp != prev_lamp || occ != prev_occ || r->mode != prev_mode)
            ESP_LOGI(TAG, "room %d: occupied=%d lamp=%d mode=%s light=%d", id, occ, r->lamp, MODE_NAME[r->mode], raw);
        prev_lamp = r->lamp; prev_occ = occ; prev_mode = r->mode;
        xSemaphoreGive(r->lock);

        if (alert && now - last_alert > 30000000) {         /* at most one alert per 30 s */
            log_sys("SECURITY ALERT room %d: %s", id, alert);
            door_beep(2);
            last_alert = now;
        }
    }
}

void rooms_init(void)
{
    const int pir[NUM_ROOMS] = { PIN_PIR_1, PIN_PIR_2 }, lamp[NUM_ROOMS] = { PIN_LAMP_1, PIN_LAMP_2 };
    const adc_channel_t ch[NUM_ROOMS] = { ADC_CH_ROOM_1, ADC_CH_ROOM_2 };

    adc_oneshot_unit_init_cfg_t u = { .unit_id = ADC_UNIT_1 };          /* ADC1: ADC2 cannot be used with Wi-Fi */
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&u, &s_adc));
    adc_oneshot_chan_cfg_t c = { .bitwidth = ADC_BITWIDTH_DEFAULT, .atten = ADC_ATTEN_DB_12 };

    for (int i = 0; i < NUM_ROOMS; i++) {
        room_t *r = &s_room[i];
        r->pir_pin = pir[i]; r->lamp_pin = lamp[i]; r->ldr = ch[i];
        ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, ch[i], &c));
        gpio_config_t o = { .pin_bit_mask = 1ULL << lamp[i], .mode = GPIO_MODE_OUTPUT };
        gpio_config(&o); gpio_set_level(lamp[i], 0);
        gpio_config_t in = { .pin_bit_mask = 1ULL << pir[i], .mode = GPIO_MODE_INPUT, .intr_type = GPIO_INTR_ANYEDGE };
        gpio_config(&in);
        r->lock = xSemaphoreCreateMutex();
        r->vacancy = xTimerCreate("vacancy", pdMS_TO_TICKS(OCC_HOLD_MS), pdFALSE, r, vacancy_cb);
        configASSERT(r->lock && r->vacancy);
        xTaskCreate(room_task, i == 0 ? "room1" : "room2", 4096, r, 5, &r->task);
    }
    /* the GPIO ISR service is installed in app_main; attach the PIR handlers now that tasks exist */
    for (int i = 0; i < NUM_ROOMS; i++) gpio_isr_handler_add(s_room[i].pir_pin, pir_isr, &s_room[i]);
}

bool room_set_mode(int room, const char *mode)
{
    if (room < 1 || room > NUM_ROOMS) return false;
    for (int m = 0; m < 3; m++) if (!strcmp(mode, MODE_NAME[m])) {
        room_t *r = &s_room[room - 1];
        xSemaphoreTake(r->lock, portMAX_DELAY); r->mode = (light_mode_t)m; xSemaphoreGive(r->lock);
        xTaskNotifyGive(r->task);
        return true;
    }
    return false;
}

void rooms_energy(float *wh, float *saved)
{
    *wh = *saved = 0;
    for (int i = 0; i < NUM_ROOMS; i++) {
        room_t *r = &s_room[i];
        xSemaphoreTake(r->lock, portMAX_DELAY);
        *wh    += (float)r->on_s * LAMP_WATTS / 3600.0f;
        *saved += (float)(r->up_s - r->on_s) * LAMP_WATTS / 3600.0f;       /* vs lamp left on all the time */
        xSemaphoreGive(r->lock);
    }
}

void rooms_print(void)
{
    for (int i = 0; i < NUM_ROOMS; i++) {
        room_t *r = &s_room[i];
        xSemaphoreTake(r->lock, portMAX_DELAY);
        printf("  Room %d : occupied=%-3s lamp=%-3s mode=%-4s light-level=%-4d lamp-on=%lus energy=%.2fWh saved~%.2fWh\n",
               i + 1, r->occupied ? "YES" : "no", r->lamp ? "ON" : "off", MODE_NAME[r->mode], r->raw,
               (unsigned long)r->on_s, (float)r->on_s * LAMP_WATTS / 3600.0f, (float)(r->up_s - r->on_s) * LAMP_WATTS / 3600.0f);
        xSemaphoreGive(r->lock);
    }
}
