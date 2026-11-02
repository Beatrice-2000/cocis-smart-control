#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "app_config.h"
#include "app.h"
#include "rfid.h"
#include "door.h"
#include "store.h"
#include "access.h"
#include "auth.h"
#include "timesync.h"
#include "rooms.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

static const char *TAG = "main";

typedef struct { reader_t reader; char uid[15]; } scan_t;

static QueueHandle_t s_q_scans;
static SemaphoreHandle_t s_emerg_sem;
static rfid_t s_rfid[2];

/* ====================== status for the serial menu ====================== */
void status_print(void)
{
    char ts[32]; fmt_timestamp(ts, sizeof ts);
    int o, c; sched_get(&o, &c);
    printf("  Time        : %s\n", ts);
    printf("  Access      : %02d:%02d-%02d:%02d  now %s\n", o / 60, o % 60, c / 60, c % 60, sched_is_open() ? "OPEN" : "CLOSED");
    printf("  Occupancy   : %d inside\n", access_occupancy());
    printf("  SD card     : %s\n", store_sd_ok() ? "OK" : "NOT AVAILABLE (degraded mode)");
    printf("  Wi-Fi       : %s   clock %s\n", ts_wifi_up() ? "up" : "down", time_valid() ? "valid" : "NOT set");
    printf("  2FA         : %s\n  Logged in   : %s\n", cfg_get_2fa() ? "ON" : "off", auth_level() ? auth_user() : "nobody");
    rooms_print();
}

bool scan_inject(int reader, const char *uid)      /* virtual card tap from the serial menu (admin, logged) */
{
    scan_t s; s.reader = reader ? READER_OUT : READER_IN;
    int n = 0;                                          /* normalise: 01:02:03:04 / 01020304 / lower case */
    for (const char *p = uid; *p && n < 14; p++) if (isxdigit((unsigned char)*p)) s.uid[n++] = toupper((unsigned char)*p);
    s.uid[n] = 0;
    return xQueueSend(s_q_scans, &s, 0) == pdTRUE;
}

/* ====================== tasks ====================== */
static void rfid_task(void *arg)
{
    reader_t rd = (reader_t)(intptr_t)arg;
    char last[15] = ""; int64_t last_us = 0;
    for (;;) {
        uint8_t uid[10], len = 0;
        if (rfid_read_uid(&s_rfid[rd], uid, &len)) {
            scan_t s; s.reader = rd; uid_to_str(uid, len, s.uid);
            int64_t now = esp_timer_get_time();
            if (strcmp(s.uid, last) || now - last_us > SCAN_COOLDOWN_MS * 1000LL) {     /* ignore repeats of the same tap */
                xQueueSend(s_q_scans, &s, 0);
                strlcpy(last, s.uid, sizeof last);
            }
            last_us = now;
        }
        vTaskDelay(pdMS_TO_TICKS(150));
    }
}

static void access_task(void *arg)
{
    scan_t s; int unknown_cnt[2] = { 0, 0 }; int64_t unknown_t0[2] = { 0, 0 };
    for (;;) {
        if (xQueueReceive(s_q_scans, &s, portMAX_DELAY) != pdTRUE) continue;
        char name[24];
        ar_t res = access_process(s.reader, s.uid, name, sizeof name);   /* atomic check-and-update */
        int occ = access_occupancy();
        const char *dir = s.reader == READER_IN ? "IN" : "OUT";
        door_id_t d = s.reader == READER_IN ? DOOR_IN : DOOR_OUT;

        if (ar_granted(res)) door_act(d, ACT_GRANT);
        else if (res == AR_ENROLLED) door_beep(2);
        else door_act(d, ACT_DENY);

        printf("[%-5s] %-14s %-22s -> %s (inside: %d)\n", dir, s.uid, name[0] ? name : "unknown", ar_str(res), occ);
        log_access("%s,%s,%s,%s,%d", s.uid, name[0] ? name : "UNKNOWN", dir, ar_str(res), occ);

        /* 3 unknown cards within a minute at the same reader = possible attack */
        if (res == AR_DENIED_UNKNOWN) {
            int64_t now = esp_timer_get_time();
            if (now - unknown_t0[s.reader] > 60LL * 1000000) { unknown_t0[s.reader] = now; unknown_cnt[s.reader] = 0; }
            if (++unknown_cnt[s.reader] >= 3) {
                log_sys("SECURITY: %d unknown cards at the %s reader within 1 minute", unknown_cnt[s.reader], dir);
                door_act(d, ACT_ALARM);
                unknown_cnt[s.reader] = 0;
            }
        }
    }
}

static void IRAM_ATTR emergency_isr(void *arg)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_emerg_sem, &woken);
    if (woken) portYIELD_FROM_ISR();
}
static void emergency_task(void *arg)
{
    for (;;) {
        xSemaphoreTake(s_emerg_sem, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(50));                                   /* debounce */
        if (gpio_get_level(PIN_EMERGENCY) != 0) continue;
        door_act(DOOR_IN, ACT_GRANT); door_act(DOOR_OUT, ACT_GRANT);     /* safety first: open both doors */
        log_sys("EMERGENCY RELEASE button pressed - both doors unlocked");
        log_access("-,EMERGENCY,BOTH,EMERGENCY_RELEASE,%d", access_occupancy());
        vTaskDelay(pdMS_TO_TICKS(3000));
        xSemaphoreTake(s_emerg_sem, 0);                                  /* drop bounces that arrived meanwhile */
    }
}

static void monitor_task(void *arg)
{
    bool prev_open = sched_is_open(), prev_wifi = false, prev_clock = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        bool open = sched_is_open();
        if (open != prev_open) {
            int inside = access_occupancy();
            log_sys("Access window is now %s (%d inside)", open ? "OPEN" : "CLOSED", inside);
            if (!open && inside > 0) log_sys("WARNING: %d person(s) still inside at closing time", inside);
            prev_open = open;
        }
        bool w = ts_wifi_up(), c = time_valid();
        if (w != prev_wifi) { log_sys("Wi-Fi %s", w ? "connected" : "lost (system keeps working locally)"); prev_wifi = w; }
        if (c != prev_clock) { log_sys("Clock %s", c ? "is now valid" : "invalid"); prev_clock = c; }
    }
}

void app_main(void)
{
    setenv("TZ", "EAT-3", 1); tzset();                                  /* East Africa Time */
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) { ESP_ERROR_CHECK(nvs_flash_erase()); ESP_ERROR_CHECK(nvs_flash_init()); }

    /* shared hardware first: the SPI bus is used by RFID readers AND the SD card */
    ESP_ERROR_CHECK(rfid_bus_init());
    store_init();
    access_init();
    auth_init();
    door_init();
    ESP_ERROR_CHECK(rfid_init(&s_rfid[READER_IN],  PIN_RFID_CS_IN));
    ESP_ERROR_CHECK(rfid_init(&s_rfid[READER_OUT], PIN_RFID_CS_OUT));

    /* synchronisation objects before any task uses them */
    s_q_scans = xQueueCreate(8, sizeof(scan_t));
    s_emerg_sem = xSemaphoreCreateBinary();
    configASSERT(s_q_scans && s_emerg_sem);

    gpio_config_t eb = { .pin_bit_mask = 1ULL << PIN_EMERGENCY, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE, .intr_type = GPIO_INTR_NEGEDGE };
    gpio_config(&eb);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(PIN_EMERGENCY, emergency_isr, NULL);

    xTaskCreate(access_task,    "access",  4096, NULL, 6, NULL);
    xTaskCreate(rfid_task,      "rfid_in", 3072, (void *)(intptr_t)READER_IN,  5, NULL);
    xTaskCreate(rfid_task,      "rfid_out",3072, (void *)(intptr_t)READER_OUT, 5, NULL);
    xTaskCreate(emergency_task, "emerg",   3072, NULL, 7, NULL);
    xTaskCreate(monitor_task,   "monitor", 4096, NULL, 4, NULL);

    rooms_init();                                    /* needs the GPIO ISR service installed above */
    ts_start();                                      /* Wi-Fi + SNTP time in the background */

    cli_start();
    log_sys("CoCIS smart control started (SD %s)", store_sd_ok() ? "ok" : "missing");
    ESP_LOGI(TAG, "ready");
}
