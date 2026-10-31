/*
 * Access control rules, occupancy tracking, user database and the access schedule.
 *
 * SYNCHRONISATION (requirement 2.7)
 *   s_ulock : protects the user table AND the "inside" flags. The check
 *             "is this person already inside?" and the change "mark inside" happen in ONE
 *             locked block, so two readers (or the CLI) can never interleave and corrupt the
 *             occupancy. This removes the race condition.
 *   s_clock : protects the schedule and the configuration flags.
 *   Lock order rule: s_ulock -> SD lock. (store.c never takes s_ulock, so no deadlock.)
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>
#include "access.h"
#include "store.h"
#include "timesync.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "access";
#define USERS_FILE  "/sdcard/users.csv"
#define INSIDE_FILE "/sdcard/inside.csv"

static user_t s_users[MAX_USERS];
static int    s_nusers;
static SemaphoreHandle_t s_ulock, s_clock;
static int    s_open_min = 7 * 60, s_close_min = 20 * 60;
static bool   s_need_2fa;
static int64_t s_admin_seen_us;
static bool   s_enroll_armed; static char s_enroll_name[24]; static role_t s_enroll_role; static int64_t s_enroll_deadline_us;
static int s_st_in, s_st_out, s_st_denied, s_st_anomaly, s_st_peak;

/* default demo users = the preset cards of the Wokwi MFRC522 (used if users.csv is missing) */
static const struct { const char *uid, *name; role_t role; } DEFAULTS[] = {
    { "01020304", "Alice (Blue card)",  ROLE_ADMIN   },
    { "11223344", "Bob (Green card)",   ROLE_STAFF   },
    { "55667788", "Carol (Yellow card)", ROLE_STUDENT },
    { "04112233445566", "Dan (NFC tag)", ROLE_STUDENT },
    { "C0FFEE99", "Eve (Key fob)",      ROLE_STAFF   },
    /* AABBCCDD (Red card) is deliberately NOT registered -> used to demonstrate "denied" */
};

const char *role_str(role_t r) { return r == ROLE_ADMIN ? "ADMIN" : r == ROLE_STAFF ? "STAFF" : "STUDENT"; }
bool role_parse(const char *s, role_t *r)
{
    if (!strcasecmp(s, "admin"))   { *r = ROLE_ADMIN;   return true; }
    if (!strcasecmp(s, "staff"))   { *r = ROLE_STAFF;   return true; }
    if (!strcasecmp(s, "student")) { *r = ROLE_STUDENT; return true; }
    return false;
}
const char *ar_str(ar_t r)
{
    static const char *N[] = { "GRANTED", "GRANTED_ANOMALY(no entry record)", "DENIED_UNKNOWN_CARD", "DENIED_CARD_BLOCKED",
                               "DENIED_OUTSIDE_HOURS", "DENIED_ALREADY_INSIDE", "NEW_USER_ENROLLED" };
    return N[r];
}
bool ar_granted(ar_t r) { return r == AR_GRANTED || r == AR_GRANTED_ANOMALY; }

void uid_to_str(const uint8_t *uid, uint8_t len, char out[15])
{
    for (int i = 0; i < len && i < 7; i++) sprintf(&out[i * 2], "%02X", uid[i]);
    out[len * 2] = 0;
}
static void norm_uid(const char *in, char out[15])           /* "01:02:03:04" / "01020304" -> "01020304" */
{
    int n = 0;
    for (; *in && n < 14; in++) if (isxdigit((unsigned char)*in)) out[n++] = toupper((unsigned char)*in);
    out[n] = 0;
}

/* ---------------- persistence (SD card). Caller holds s_ulock ---------------- */
static void save_users_locked(void)
{
    if (!store_sd_ok()) return;
    store_lock();
    FILE *f = fopen(USERS_FILE, "w");
    if (f) {
        fprintf(f, "uid,name,role,active\n");
        for (int i = 0; i < s_nusers; i++)
            fprintf(f, "%s,%s,%s,%d\n", s_users[i].uid, s_users[i].name, role_str(s_users[i].role), s_users[i].active);
        fclose(f);
    }
    store_unlock();
}
static void save_inside_locked(void)
{
    if (!store_sd_ok()) return;
    store_lock();
    FILE *f = fopen(INSIDE_FILE, "w");
    if (f) { for (int i = 0; i < s_nusers; i++) if (s_users[i].inside) fprintf(f, "%s\n", s_users[i].uid); fclose(f); }
    store_unlock();
}
static user_t *find_locked(const char *uid)
{
    for (int i = 0; i < s_nusers; i++) if (!strcmp(s_users[i].uid, uid)) return &s_users[i];
    return NULL;
}
static void load_users(void)
{
    s_nusers = 0;
    bool loaded = false;
    if (store_sd_ok()) {
        store_lock();
        FILE *f = fopen(USERS_FILE, "r");
        if (f) {
            char line[96];
            while (fgets(line, sizeof line, f) && s_nusers < MAX_USERS) {
                line[strcspn(line, "\r\n")] = 0;
                char *sv, *uid = strtok_r(line, ",", &sv), *name = strtok_r(NULL, ",", &sv),
                     *role = strtok_r(NULL, ",", &sv), *act = strtok_r(NULL, ",", &sv);
                role_t r;
                if (!uid || !name || !role || !role_parse(role, &r)) continue;   /* skips the header line */
                user_t *u = &s_users[s_nusers++];
                memset(u, 0, sizeof *u);
                norm_uid(uid, u->uid); strlcpy(u->name, name, sizeof u->name);
                u->role = r; u->active = act ? atoi(act) != 0 : true;
            }
            fclose(f); loaded = true;
        }
        store_unlock();
    }
    if (!loaded || s_nusers == 0) {                                     /* fall back to built-in demo users */
        for (size_t i = 0; i < sizeof DEFAULTS / sizeof DEFAULTS[0]; i++) {
            user_t *u = &s_users[s_nusers++];
            memset(u, 0, sizeof *u);
            strlcpy(u->uid, DEFAULTS[i].uid, sizeof u->uid); strlcpy(u->name, DEFAULTS[i].name, sizeof u->name);
            u->role = DEFAULTS[i].role; u->active = true;
        }
        save_users_locked();
    }
    /* restore who was inside before a reboot / power cut */
    if (store_sd_ok()) {
        store_lock();
        FILE *f = fopen(INSIDE_FILE, "r");
        if (f) {
            char line[32];
            while (fgets(line, sizeof line, f)) {
                line[strcspn(line, "\r\n")] = 0;
                user_t *u = find_locked(line);
                if (u) u->inside = true;
            }
            fclose(f);
        }
        store_unlock();
    }
}

/* ---------------- configuration in NVS (survives resets) ---------------- */
static void cfg_load(void)
{
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READONLY, &h) == ESP_OK) {
        int32_t v;
        if (nvs_get_i32(h, "open", &v) == ESP_OK)  s_open_min = v;
        if (nvs_get_i32(h, "close", &v) == ESP_OK) s_close_min = v;
        if (nvs_get_i32(h, "twofa", &v) == ESP_OK) s_need_2fa = v != 0;
        nvs_close(h);
    }
}
static void cfg_save(void)
{
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "open", s_open_min); nvs_set_i32(h, "close", s_close_min);
        nvs_set_i32(h, "twofa", s_need_2fa); nvs_commit(h); nvs_close(h);
    }
}

void access_init(void)
{
    s_ulock = xSemaphoreCreateMutex();
    s_clock = xSemaphoreCreateMutex();
    cfg_load();
    xSemaphoreTake(s_ulock, portMAX_DELAY);
    load_users();
    int inside = 0; for (int i = 0; i < s_nusers; i++) inside += s_users[i].inside;
    xSemaphoreGive(s_ulock);
    ESP_LOGI(TAG, "%d users loaded, %d inside, access window %02d:%02d-%02d:%02d",
             s_nusers, inside, s_open_min / 60, s_open_min % 60, s_close_min / 60, s_close_min % 60);
}

/* ---------------- schedule ---------------- */
bool sched_parse_hhmm(const char *s, int *m)
{
    int h, mi;
    if (sscanf(s, "%d:%d", &h, &mi) != 2 || h < 0 || h > 23 || mi < 0 || mi > 59) return false;
    *m = h * 60 + mi; return true;
}
void sched_get(int *o, int *c)
{
    xSemaphoreTake(s_clock, portMAX_DELAY); *o = s_open_min; *c = s_close_min; xSemaphoreGive(s_clock);
}
bool sched_set(int o, int c)
{
    if (o < 0 || o > 1439 || c < 0 || c > 1439) return false;
    xSemaphoreTake(s_clock, portMAX_DELAY); s_open_min = o; s_close_min = c; cfg_save(); xSemaphoreGive(s_clock);
    return true;
}
bool sched_is_open(void)
{
    struct tm tm;
    if (!time_get_local(&tm)) return false;            /* FAIL-SECURE: unknown time = closed */
    int now = tm.tm_hour * 60 + tm.tm_min, o, c;
    sched_get(&o, &c);
    if (o == c) return true;                               /* equal times = open 24 h */
    return o < c ? (now >= o && now < c) : (now >= o || now < c);   /* second form = overnight window */
}
bool cfg_get_2fa(void) { xSemaphoreTake(s_clock, portMAX_DELAY); bool v = s_need_2fa; xSemaphoreGive(s_clock); return v; }
void cfg_set_2fa(bool on) { xSemaphoreTake(s_clock, portMAX_DELAY); s_need_2fa = on; cfg_save(); xSemaphoreGive(s_clock); }

/* ---------------- the decision ---------------- */
ar_t access_process(reader_t rd, const char *uid, char *name_out, size_t nlen)
{
    bool open = sched_is_open();                           /* uses s_clock, released before s_ulock */
    ar_t res;
    name_out[0] = 0;

    xSemaphoreTake(s_ulock, portMAX_DELAY);
    user_t *u = find_locked(uid);
    if (!u) {
        if (s_enroll_armed && esp_timer_get_time() < s_enroll_deadline_us && s_nusers < MAX_USERS) {
            u = &s_users[s_nusers++];
            memset(u, 0, sizeof *u);
            strlcpy(u->uid, uid, sizeof u->uid); strlcpy(u->name, s_enroll_name, sizeof u->name);
            u->role = s_enroll_role; u->active = true;
            s_enroll_armed = false;
            save_users_locked();
            strlcpy(name_out, u->name, nlen);
            res = AR_ENROLLED;
        } else {
            res = AR_DENIED_UNKNOWN;
        }
    } else {
        strlcpy(name_out, u->name, nlen);
        if (u->role == ROLE_ADMIN && u->active) s_admin_seen_us = esp_timer_get_time();   /* for optional 2FA */
        if (!u->active) {
            res = AR_DENIED_BLOCKED;
        } else if (!open) {
            res = AR_DENIED_SCHEDULE;                      /* applies to BOTH entry and exit doors */
        } else if (rd == READER_IN) {
            if (u->inside) res = AR_DENIED_PASSBACK;       /* anti-passback: already inside, card shared? */
            else { u->inside = true; res = AR_GRANTED; save_inside_locked(); }
        } else {
            if (!u->inside) res = AR_GRANTED_ANOMALY;      /* let them out (safety) but flag it */
            else { u->inside = false; res = AR_GRANTED; save_inside_locked(); }
        }
    }
    /* statistics, still under the same lock */
    if (res == AR_GRANTED || res == AR_GRANTED_ANOMALY) { if (rd == READER_IN) s_st_in++; else s_st_out++; }
    else if (res != AR_ENROLLED) s_st_denied++;
    if (res == AR_GRANTED_ANOMALY) s_st_anomaly++;
    int occ = 0; for (int i = 0; i < s_nusers; i++) occ += s_users[i].inside;
    if (occ > s_st_peak) s_st_peak = occ;
    xSemaphoreGive(s_ulock);
    return res;
}

/* ---------------- queries used by the CLI and MQTT ---------------- */
int access_occupancy(void)
{
    xSemaphoreTake(s_ulock, portMAX_DELAY);
    int n = 0; for (int i = 0; i < s_nusers; i++) n += s_users[i].inside;
    xSemaphoreGive(s_ulock);
    return n;
}
void access_print_inside(void)
{
    xSemaphoreTake(s_ulock, portMAX_DELAY);
    int n = 0;
    for (int i = 0; i < s_nusers; i++) if (s_users[i].inside) { printf("  %-16s %-26s %s\n", s_users[i].uid, s_users[i].name, role_str(s_users[i].role)); n++; }
    xSemaphoreGive(s_ulock);
    printf("  -> %d person(s) inside\n", n);
}
void access_stats(int *in, int *out, int *denied, int *anomaly, int *peak)
{
    xSemaphoreTake(s_ulock, portMAX_DELAY);
    *in = s_st_in; *out = s_st_out; *denied = s_st_denied; *anomaly = s_st_anomaly; *peak = s_st_peak;
    xSemaphoreGive(s_ulock);
}
bool access_admin_card_recent(int seconds)
{
    xSemaphoreTake(s_ulock, portMAX_DELAY);
    bool ok = s_admin_seen_us != 0 && (esp_timer_get_time() - s_admin_seen_us) < (int64_t)seconds * 1000000;
    xSemaphoreGive(s_ulock);
    return ok;
}

/* ---------------- user management ---------------- */
int user_add(const char *uid_in, const char *name, role_t role)
{
    char uid[15]; norm_uid(uid_in, uid);
    if (strlen(uid) != 8 && strlen(uid) != 14) return -1;               /* 4 or 7 bytes */
    xSemaphoreTake(s_ulock, portMAX_DELAY);
    int r = 0;
    if (find_locked(uid)) r = -2;
    else if (s_nusers >= MAX_USERS) r = -3;
    else {
        user_t *u = &s_users[s_nusers++]; memset(u, 0, sizeof *u);
        strlcpy(u->uid, uid, sizeof u->uid); strlcpy(u->name, name, sizeof u->name); u->role = role; u->active = true;
        save_users_locked();
    }
    xSemaphoreGive(s_ulock);
    return r;
}
int user_del(const char *uid_in)
{
    char uid[15]; norm_uid(uid_in, uid);
    xSemaphoreTake(s_ulock, portMAX_DELAY);
    int r = -1;
    for (int i = 0; i < s_nusers; i++) if (!strcmp(s_users[i].uid, uid)) {
        bool was_inside = s_users[i].inside;
        s_users[i] = s_users[--s_nusers]; r = 0;
        save_users_locked(); if (was_inside) save_inside_locked();
        break;
    }
    xSemaphoreGive(s_ulock);
    return r;
}
int user_set_active(const char *uid_in, bool active)
{
    char uid[15]; norm_uid(uid_in, uid);
    xSemaphoreTake(s_ulock, portMAX_DELAY);
    user_t *u = find_locked(uid); int r = u ? 0 : -1;
    if (u) { u->active = active; save_users_locked(); }
    xSemaphoreGive(s_ulock);
    return r;
}
void users_print(void)
{
    xSemaphoreTake(s_ulock, portMAX_DELAY);
    for (int i = 0; i < s_nusers; i++)
        printf("  %-16s %-26s %-8s %-8s %s\n", s_users[i].uid, s_users[i].name, role_str(s_users[i].role),
               s_users[i].active ? "active" : "BLOCKED", s_users[i].inside ? "INSIDE" : "");
    printf("  -> %d registered user(s)\n", s_nusers);
    xSemaphoreGive(s_ulock);
}
bool enroll_arm(const char *name, role_t role)
{
    xSemaphoreTake(s_ulock, portMAX_DELAY);
    strlcpy(s_enroll_name, name, sizeof s_enroll_name); s_enroll_role = role;
    s_enroll_armed = true; s_enroll_deadline_us = esp_timer_get_time() + 30LL * 1000000;
    xSemaphoreGive(s_ulock);
    return true;
}
