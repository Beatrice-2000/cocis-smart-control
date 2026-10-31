#include <stdlib.h>
#include <string.h>
#include "timesync.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_sntp.h"

#define WIFI_SSID "Wokwi-GUEST"          /* Wokwi's free virtual access point */
static const char *TAG = "time";
static volatile bool s_up, s_sntp;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_up = false; esp_wifi_connect();                         /* keep retrying forever */
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_up = true;
        ESP_LOGI(TAG, "Wi-Fi up, starting SNTP");
        if (!s_sntp) {
            s_sntp = true;
            esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
            esp_sntp_setservername(0, "pool.ntp.org");
            esp_sntp_init();
        }
    }
}

void ts_start(void)
{
    setenv("TZ", "EAT-3", 1); tzset();                            /* East Africa Time = UTC+3 */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t w = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&w));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));
    wifi_config_t sta = { 0 };
    strlcpy((char *)sta.sta.ssid, WIFI_SSID, sizeof sta.sta.ssid);
    sta.sta.threshold.authmode = WIFI_AUTH_OPEN;
    sta.sta.channel = 6;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    ESP_ERROR_CHECK(esp_wifi_start());
}

bool ts_wifi_up(void) { return s_up; }
bool time_valid(void) { return time(NULL) > 1735689600; }         /* later than 1 Jan 2025 */
bool time_get_local(struct tm *out) { time_t t = time(NULL); localtime_r(&t, out); return time_valid(); }
