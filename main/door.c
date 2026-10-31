/*
 * Door mechanisms: one servo (lock), one green + one red LED per door, shared buzzer.
 * Each door has its OWN task and queue, so the entry door and exit door work independently.
 * The buzzer is shared by both tasks, so it is protected by a mutex.
 */
#include "door.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

static const int SERVO_PIN[2] = { PIN_SERVO_IN, PIN_SERVO_OUT };
static const int GREEN[2]     = { PIN_LED_IN_G, PIN_LED_OUT_G };
static const int RED[2]       = { PIN_LED_IN_R, PIN_LED_OUT_R };
static const ledc_channel_t SERVO_CH[2] = { LEDC_CHANNEL_0, LEDC_CHANNEL_1 };
static QueueHandle_t s_q[2];
static SemaphoreHandle_t s_buz_lock;

static void servo_angle(int id, int deg)
{
    uint32_t us = 500 + (uint32_t)deg * 2000 / 180;          /* 0.5 ms .. 2.5 ms pulse */
    uint32_t duty = us * 16384 / 20000;                       /* 14-bit duty at 50 Hz  */
    ledc_set_duty(LEDC_LOW_SPEED_MODE, SERVO_CH[id], duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, SERVO_CH[id]);
}

void door_beep(int times)
{
    xSemaphoreTake(s_buz_lock, portMAX_DELAY);               /* only one door may use the buzzer at a time */
    for (int i = 0; i < times; i++) {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2, 512);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2);
        vTaskDelay(pdMS_TO_TICKS(90));
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2, 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2);
        vTaskDelay(pdMS_TO_TICKS(90));
    }
    xSemaphoreGive(s_buz_lock);
}

static void door_task(void *arg)
{
    int id = (int)(intptr_t)arg;
    door_act_t a;
    for (;;) {
        if (xQueueReceive(s_q[id], &a, portMAX_DELAY) != pdTRUE) continue;
        switch (a) {
        case ACT_GRANT:
            gpio_set_level(GREEN[id], 1);
            servo_angle(id, 90);                              /* unlock */
            door_beep(1);
            vTaskDelay(pdMS_TO_TICKS(DOOR_OPEN_MS));
            servo_angle(id, 0);                               /* lock again */
            gpio_set_level(GREEN[id], 0);
            break;
        case ACT_DENY:
            gpio_set_level(RED[id], 1);
            door_beep(3);
            vTaskDelay(pdMS_TO_TICKS(800));
            gpio_set_level(RED[id], 0);
            break;
        case ACT_ALARM:
            for (int i = 0; i < 6; i++) {
                gpio_set_level(RED[id], 1); door_beep(1);
                gpio_set_level(RED[id], 0); vTaskDelay(pdMS_TO_TICKS(120));
            }
            break;
        }
    }
}

void door_act(door_id_t d, door_act_t a) { xQueueSend(s_q[d], &a, 0); }

void door_init(void)
{
    uint64_t mask = 0;
    for (int i = 0; i < 2; i++) mask |= (1ULL << GREEN[i]) | (1ULL << RED[i]);
    gpio_config_t o = { .pin_bit_mask = mask, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&o);

    ledc_timer_config_t t0 = { .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_14_BIT,
                               .timer_num = LEDC_TIMER_0, .freq_hz = 50, .clk_cfg = LEDC_AUTO_CLK };
    ledc_timer_config(&t0);
    ledc_timer_config_t t1 = { .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_10_BIT,
                               .timer_num = LEDC_TIMER_1, .freq_hz = 2000, .clk_cfg = LEDC_AUTO_CLK };
    ledc_timer_config(&t1);
    for (int i = 0; i < 2; i++) {
        ledc_channel_config_t c = { .gpio_num = SERVO_PIN[i], .speed_mode = LEDC_LOW_SPEED_MODE,
                                    .channel = SERVO_CH[i], .timer_sel = LEDC_TIMER_0, .duty = 0, .hpoint = 0 };
        ledc_channel_config(&c);
        servo_angle(i, 0);                                    /* start LOCKED */
    }
    ledc_channel_config_t b = { .gpio_num = PIN_BUZZER, .speed_mode = LEDC_LOW_SPEED_MODE,
                                .channel = LEDC_CHANNEL_2, .timer_sel = LEDC_TIMER_1, .duty = 0, .hpoint = 0 };
    ledc_channel_config(&b);

    s_buz_lock = xSemaphoreCreateMutex();
    for (int i = 0; i < 2; i++) {
        s_q[i] = xQueueCreate(4, sizeof(door_act_t));
        xTaskCreate(door_task, i == 0 ? "door_in" : "door_out", 3072, (void *)(intptr_t)i, 4, NULL);
    }
}
