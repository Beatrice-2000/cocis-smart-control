#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "store.h"
#include "app_config.h"
#include "timesync.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#define LINE_LEN 160
#define RING_N   40
static const char *TAG = "store";
static bool s_sd_ok;
static sdmmc_card_t *s_card;
static SemaphoreHandle_t s_sd_lock, s_ring_lock;
static QueueHandle_t s_logq;
static FILE *s_f_access, *s_f_sys;
static char s_ring[RING_N][LINE_LEN];
static int  s_ring_count, s_ring_head;
static volatile uint32_t s_dropped;

typedef struct { uint8_t kind; char line[LINE_LEN]; } logmsg_t;   /* kind 0 = access, 1 = system */

bool store_sd_ok(void)  { return s_sd_ok; }
void store_lock(void)   { xSemaphoreTake(s_sd_lock, portMAX_DELAY); }
void store_unlock(void) { xSemaphoreGive(s_sd_lock); }

void fmt_timestamp(char *buf, size_t n)
{
    struct tm tm;
    if (time_get_local(&tm)) strftime(buf, n, "%Y-%m-%d %H:%M:%S", &tm);
    else snprintf(buf, n, "uptime+%lus", (unsigned long)(xTaskGetTickCount() / configTICK_RATE_HZ));
}

static void vlog(uint8_t kind, const char *fmt, va_list ap)
{
    logmsg_t m; m.kind = kind;
    char ts[24], body[LINE_LEN - 30];
    fmt_timestamp(ts, sizeof ts);
    vsnprintf(body, sizeof body, fmt, ap);
    snprintf(m.line, sizeof m.line, "%s,%s", ts, body);
    if (xQueueSend(s_logq, &m, 0) != pdTRUE) s_dropped++;      /* never block the caller */
}
void log_access(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vlog(0, fmt, ap); va_end(ap); }
void log_sys(const char *fmt, ...)    { va_list ap; va_start(ap, fmt); vlog(1, fmt, ap); va_end(ap); }

int log_recent(char out[][LINE_LEN], int max)
{
    xSemaphoreTake(s_ring_lock, portMAX_DELAY);
    int n = s_ring_count < max ? s_ring_count : max;
    for (int i = 0; i < n; i++) {                              /* oldest -> newest */
        int idx = (s_ring_head - n + i + RING_N * 2) % RING_N;
        strlcpy(out[i], s_ring[idx], LINE_LEN);
    }
    xSemaphoreGive(s_ring_lock);
    return n;
}

static void logger_task(void *arg)
{
    logmsg_t m;
    for (;;) {
        if (xQueueReceive(s_logq, &m, portMAX_DELAY) != pdTRUE) continue;
        if (m.kind == 0) {
            xSemaphoreTake(s_ring_lock, portMAX_DELAY);        /* keep a copy in RAM for the CLI */
            strlcpy(s_ring[s_ring_head], m.line, LINE_LEN);
            s_ring_head = (s_ring_head + 1) % RING_N;
            if (s_ring_count < RING_N) s_ring_count++;
            xSemaphoreGive(s_ring_lock);
        } else {
            printf("[SYS] %s\n", m.line);
        }
        if (s_sd_ok) {
            store_lock();
            FILE *f = m.kind == 0 ? s_f_access : s_f_sys;
            if (f) { fprintf(f, "%s\n", m.line); fflush(f); fsync(fileno(f)); }
            store_unlock();
        }
    }
}

static FILE *open_log(const char *path, const char *header)
{
    FILE *f = fopen(path, "r");
    bool exists = f != NULL;
    if (f) fclose(f);
    f = fopen(path, "a");
    if (f && !exists && header) { fprintf(f, "%s\n", header); fflush(f); }
    return f;
}

bool store_init(void)
{
    s_sd_lock = xSemaphoreCreateMutex();
    s_ring_lock = xSemaphoreCreateMutex();
    s_logq = xQueueCreate(24, sizeof(logmsg_t));

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI_HOST_ID;                                  /* same SPI bus as the RFID readers */
    host.max_freq_khz = 4000;                                 /* gentle clock: more reliable in the simulator */
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = PIN_SD_CS;
    slot.host_id = SPI_HOST_ID;
    esp_vfs_fat_mount_config_t mc = { .format_if_mount_failed = true, .max_files = 6, .allocation_unit_size = 16 * 1024 };

    esp_err_t e = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot, &mc, &s_card);
    if (e == ESP_OK) {
        s_sd_ok = true;
        s_f_access = open_log("/sdcard/access.csv", "timestamp,uid,name,direction,result,occupancy");
        s_f_sys    = open_log("/sdcard/system.log", NULL);
        ESP_LOGI(TAG, "SD card mounted (%s)", s_card->cid.name);
    } else {
        ESP_LOGE(TAG, "SD card NOT available (%s). 0x107=card does not answer (check wiring), 0x101/ESP_FAIL=file system problem. Running in DEGRADED mode (RAM log only)", esp_err_to_name(e));
    }
    xTaskCreate(logger_task, "logger", 4096, NULL, 3, NULL);
    return s_sd_ok;
}

void store_dump(const char *file)
{
    if (!s_sd_ok) { printf("SD card not available\n"); return; }
    char path[48]; snprintf(path, sizeof path, "/sdcard/%s", file);
    store_lock();
    FILE *f = fopen(path, "r");
    if (!f) { printf("cannot open %s\n", path); }
    else { char l[LINE_LEN + 8]; while (fgets(l, sizeof l, f)) fputs(l, stdout); fclose(f); }
    store_unlock();
}
