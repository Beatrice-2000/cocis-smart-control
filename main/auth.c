/*
 * Authentication for the serial interface.
 *  - Passwords are never stored: only SHA-256(salt + password), kept in NVS.
 *  - 3 wrong attempts lock the login for 30 s (brute-force protection).
 *  - A session ends automatically after 2 minutes without commands.
 *  - Two roles: manager (level 1) and admin (level 2).
 */
#include <string.h>
#include "auth.h"
#include "mbedtls/sha256.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define SESSION_US (120LL * 1000000)
#define LOCK_US    (30LL * 1000000)
typedef struct { const char *user; int level; const char *def_pass; } account_t;
static const account_t ACC[] = { { "admin", 2, "cocis2026" }, { "manager", 1, "manager2026" } };
#define NACC 2

static uint8_t s_salt[16];
static SemaphoreHandle_t s_lock;
static int s_cur = -1, s_fails;
static int64_t s_last_us, s_locked_until_us;

static void hash(const char *pw, uint8_t out[32])
{
    uint8_t buf[16 + 64]; size_t n = strlen(pw); if (n > 64) n = 64;
    memcpy(buf, s_salt, 16); memcpy(buf + 16, pw, n);
    mbedtls_sha256(buf, 16 + n, out, 0);
}
static bool load_hash(int i, uint8_t out[32])
{
    nvs_handle_t h; size_t len = 32; char key[16]; snprintf(key, sizeof key, "pw_%s", ACC[i].user);
    if (nvs_open("auth", NVS_READONLY, &h) != ESP_OK) return false;
    bool ok = nvs_get_blob(h, key, out, &len) == ESP_OK && len == 32;
    nvs_close(h); return ok;
}
static void store_hash(int i, const uint8_t in[32])
{
    nvs_handle_t h; char key[16]; snprintf(key, sizeof key, "pw_%s", ACC[i].user);
    if (nvs_open("auth", NVS_READWRITE, &h) == ESP_OK) { nvs_set_blob(h, key, in, 32); nvs_commit(h); nvs_close(h); }
}

void auth_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    nvs_handle_t h; size_t len = sizeof s_salt;
    if (nvs_open("auth", NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_get_blob(h, "salt", s_salt, &len) != ESP_OK || len != sizeof s_salt) {
            esp_fill_random(s_salt, sizeof s_salt); nvs_set_blob(h, "salt", s_salt, sizeof s_salt); nvs_commit(h);
        }
        nvs_close(h);
    }
    for (int i = 0; i < NACC; i++) {                          /* first boot: store the default passwords (hashed) */
        uint8_t hh[32];
        if (!load_hash(i, hh)) { hash(ACC[i].def_pass, hh); store_hash(i, hh); }
    }
}

bool auth_login(const char *user, const char *pass, char *msg, size_t n)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int64_t now = esp_timer_get_time();
    bool ok = false;
    if (now < s_locked_until_us) {
        snprintf(msg, n, "Too many failed attempts. Try again in %d s.", (int)((s_locked_until_us - now) / 1000000) + 1);
    } else {
        int idx = -1; for (int i = 0; i < NACC; i++) if (!strcmp(ACC[i].user, user)) idx = i;
        uint8_t want[32] = { 0 }, got[32]; hash(pass, got);
        bool have = idx >= 0 && load_hash(idx, want);
        uint8_t diff = 0; for (int i = 0; i < 32; i++) diff |= want[i] ^ got[i];   /* constant-time compare */
        if (have && diff == 0) {
            s_cur = idx; s_fails = 0; s_last_us = now; ok = true;
            snprintf(msg, n, "Welcome %s (access level %d).", ACC[idx].user, ACC[idx].level);
        } else {
            if (++s_fails >= 3) { s_locked_until_us = now + LOCK_US; s_fails = 0; snprintf(msg, n, "Login failed. Locked for 30 s."); }
            else snprintf(msg, n, "Login failed (%d/3).", s_fails);
        }
    }
    xSemaphoreGive(s_lock);
    return ok;
}
void auth_logout(void) { xSemaphoreTake(s_lock, portMAX_DELAY); s_cur = -1; xSemaphoreGive(s_lock); }

int auth_level(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int lvl = 0;
    if (s_cur >= 0) {
        int64_t now = esp_timer_get_time();
        if (now - s_last_us > SESSION_US) s_cur = -1;         /* session expired */
        else { s_last_us = now; lvl = ACC[s_cur].level; }
    }
    xSemaphoreGive(s_lock);
    return lvl;
}
const char *auth_user(void) { return s_cur >= 0 ? ACC[s_cur].user : "-"; }

bool auth_passwd(const char *np)
{
    if (strlen(np) < 6) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = s_cur >= 0;
    if (ok) { uint8_t h[32]; hash(np, h); store_hash(s_cur, h); }
    xSemaphoreGive(s_lock);
    return ok;
}
