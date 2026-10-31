/*
 * Serial interface: menus and commands.   Open the serial monitor, type  help
 * Commands are grouped by access level so that only authorised staff can change anything:
 *   level 0 = anyone       (read-only summary)
 *   level 1 = manager      (detailed information, schedule, lights)
 *   level 2 = admin        (users, time, password, 2FA, reboot)
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>
#include "app.h"
#include "rooms.h"
#include "access.h"
#include "auth.h"
#include "store.h"
#include "timesync.h"
#include "driver/uart.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef struct { const char *name; int lvl; void (*fn)(int, char **); const char *help; } cmd_t;

static bool need2fa_ok(void)
{
    if (!cfg_get_2fa() || access_admin_card_recent(60)) return true;
    printf("2FA is ON: scan an ADMIN card at the entrance (within 60 s) and repeat the command.\n");
    return false;
}

static void c_status(int ac, char **av) { status_print(); }
static void c_occ(int ac, char **av)
{
    printf("Building occupancy: %d person(s)\n", access_occupancy());
    if (auth_level() >= 1) access_print_inside();
}
static void c_rooms(int ac, char **av) { rooms_print(); }
static void c_lights(int ac, char **av)
{
    if (ac < 3) { printf("usage: lights <1|2> <auto|on|off>\n"); return; }
    int r = atoi(av[1]);
    if (!room_set_mode(r, av[2])) { printf("usage: lights <1|2> <auto|on|off>\n"); return; }
    log_sys("%s set lights room %d -> %s", auth_user(), r, av[2]);
    printf("Room %d lights set to %s.\n", r, av[2]);
}
static void c_log(int ac, char **av)
{
    static char buf[20][160];
    int n = ac > 1 ? atoi(av[1]) : 10; if (n < 1) n = 1; if (n > 20) n = 20;
    int got = log_recent(buf, n);
    printf("timestamp,uid,name,direction,result,occupancy\n");
    for (int i = 0; i < got; i++) printf("%s\n", buf[i]);
    if (!got) printf("(no events yet)\n");
}
static void c_users(int ac, char **av) { users_print(); }
static void c_adduser(int ac, char **av)
{
    role_t r;
    if (ac < 4 || !role_parse(av[3], &r)) { printf("usage: adduser <uid> <name_no_spaces> <student|staff|admin>\n"); return; }
    if (!need2fa_ok()) return;
    int e = user_add(av[1], av[2], r);
    printf(e == 0 ? "User added.\n" : e == -1 ? "UID must be 8 or 14 hex digits.\n" : e == -2 ? "UID already registered.\n" : "User table full.\n");
    if (e == 0) log_sys("%s added user %s (%s)", auth_user(), av[1], av[3]);
}
static void c_deluser(int ac, char **av)
{
    if (ac < 2) { printf("usage: deluser <uid>\n"); return; }
    if (!need2fa_ok()) return;
    printf(user_del(av[1]) == 0 ? "User deleted.\n" : "No such user.\n");
    log_sys("%s deleted user %s", auth_user(), av[1]);
}
static void c_block(int ac, char **av)
{
    if (ac < 2) { printf("usage: block <uid>   (lost or stolen card)\n"); return; }
    printf(user_set_active(av[1], false) == 0 ? "Card BLOCKED.\n" : "No such user.\n");
    log_sys("%s blocked card %s", auth_user(), av[1]);
}
static void c_unblock(int ac, char **av)
{
    if (ac < 2) { printf("usage: unblock <uid>\n"); return; }
    printf(user_set_active(av[1], true) == 0 ? "Card unblocked.\n" : "No such user.\n");
    log_sys("%s unblocked card %s", auth_user(), av[1]);
}
static void c_enroll(int ac, char **av)
{
    role_t r;
    if (ac < 3 || !role_parse(av[2], &r)) { printf("usage: enroll <name_no_spaces> <student|staff|admin>\n"); return; }
    if (!need2fa_ok()) return;
    enroll_arm(av[1], r);
    printf("Enrollment armed for 30 s: tap the NEW card on any reader.\n");
}
static void c_schedule(int ac, char **av)
{
    int o, c; sched_get(&o, &c);
    if (ac >= 2 && !strcmp(av[1], "set")) {
        if (auth_level() < 1) { printf("Permission denied: login required.\n"); return; }
        int no, nc;
        if (ac < 4 || !sched_parse_hhmm(av[2], &no) || !sched_parse_hhmm(av[3], &nc)) { printf("usage: schedule set HH:MM HH:MM\n"); return; }
        if (!need2fa_ok()) return;
        sched_set(no, nc);
        printf("Access window is now %02d:%02d - %02d:%02d\n", no / 60, no % 60, nc / 60, nc % 60);
        log_sys("%s changed access window to %02d:%02d-%02d:%02d (was %02d:%02d-%02d:%02d)", auth_user(),
                no / 60, no % 60, nc / 60, nc % 60, o / 60, o % 60, c / 60, c % 60);
        return;
    }
    printf("Access window: %02d:%02d - %02d:%02d   (now: %s)\n", o / 60, o % 60, c / 60, c % 60, sched_is_open() ? "OPEN" : "CLOSED");
    printf("Change with:   schedule set HH:MM HH:MM   (requires login)\n");
}
static void c_time(int ac, char **av)
{
    if (ac >= 4 && !strcmp(av[1], "set")) {
        if (auth_level() < 2) { printf("Permission denied: admin only.\n"); return; }
        struct tm tm = { 0 };
        if (sscanf(av[2], "%d-%d-%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday) != 3 || sscanf(av[3], "%d:%d", &tm.tm_hour, &tm.tm_min) != 2) {
            printf("usage: time set YYYY-MM-DD HH:MM\n"); return;
        }
        tm.tm_year -= 1900; tm.tm_mon -= 1; tm.tm_isdst = -1;
        struct timeval tv = { .tv_sec = mktime(&tm), .tv_usec = 0 };
        settimeofday(&tv, NULL);
        log_sys("%s set the clock manually", auth_user());
    }
    char ts[32]; fmt_timestamp(ts, sizeof ts);
    printf("Time: %s %s\n", ts, time_valid() ? "" : "(clock NOT set - use: time set YYYY-MM-DD HH:MM)");
}
static void c_login(int ac, char **av)
{
    if (ac < 3) { printf("usage: login <admin|manager> <password>\n"); return; }
    char msg[80]; bool ok = auth_login(av[1], av[2], msg, sizeof msg);
    printf("%s\n", msg);
    log_sys("login %s by %s", ok ? "OK" : "FAILED", av[1]);
}
static void c_logout(int ac, char **av) { auth_logout(); printf("Logged out.\n"); }
static void c_passwd(int ac, char **av)
{
    if (ac < 2) { printf("usage: passwd <new_password(min 6 chars)>\n"); return; }
    printf(auth_passwd(av[1]) ? "Password changed.\n" : "Password too short.\n");
    log_sys("%s changed password", auth_user());
}
static void c_stats(int ac, char **av)
{
    int in, out, den, an, pk; access_stats(&in, &out, &den, &an, &pk);
    float wh, sv; rooms_energy(&wh, &sv);
    printf("Entries granted : %d\nExits granted   : %d\nDenied attempts : %d\nAnomalies       : %d\nPeak occupancy  : %d\n", in, out, den, an, pk);
    printf("Lighting energy : %.2f Wh used, about %.2f Wh saved vs lamps always on\n", wh, sv);
}
static void c_export(int ac, char **av) { store_dump(ac > 1 && !strcmp(av[1], "system") ? "system.log" : "access.csv"); }
static void c_2fa(int ac, char **av)
{
    if (ac < 2) { printf("2FA (admin card required for config changes) is %s\n", cfg_get_2fa() ? "ON" : "OFF"); return; }
    cfg_set_2fa(!strcmp(av[1], "on"));
    printf("2FA is now %s\n", cfg_get_2fa() ? "ON" : "OFF");
    log_sys("%s set 2FA %s", auth_user(), av[1]);
}
static void c_scan(int ac, char **av)
{
    if (ac < 3 || (strcmp(av[1], "in") && strcmp(av[1], "out"))) { printf("usage: scan <in|out> <uid>   (virtual card tap, for testing)\n"); return; }
    log_sys("%s injected a VIRTUAL card tap %s at %s", auth_user(), av[2], av[1]);
    printf(scan_inject(!strcmp(av[1], "out"), av[2]) ? "Virtual tap queued.\n" : "Queue full.\n");
}
static void c_reboot(int ac, char **av) { log_sys("reboot requested by %s", auth_user()); vTaskDelay(pdMS_TO_TICKS(300)); esp_restart(); }

static void c_help(int ac, char **av);
static const cmd_t CMDS[] = {
    { "help",      0, c_help,     "help" },
    { "status",    0, c_status,   "status" },
    { "occupancy", 0, c_occ,      "occupancy" },
    { "rooms",     0, c_rooms,    "rooms" },
    { "schedule",  0, c_schedule, "schedule [set HH:MM HH:MM]" },
    { "time",      0, c_time,     "time [set YYYY-MM-DD HH:MM]" },
    { "login",     0, c_login,    "login <admin|manager> <password>" },
    { "logout",    0, c_logout,   "logout" },
    { "log",       1, c_log,      "log [n]" },
    { "stats",     1, c_stats,    "stats" },
    { "export",    1, c_export,   "export [access|system]" },
    { "lights",    1, c_lights,   "lights <1|2> <auto|on|off>" },
    { "users",     1, c_users,    "users" },
    { "block",     1, c_block,    "block <uid>" },
    { "unblock",   1, c_unblock,  "unblock <uid>" },
    { "adduser",   2, c_adduser,  "adduser <uid> <name> <role>" },
    { "deluser",   2, c_deluser,  "deluser <uid>" },
    { "enroll",    2, c_enroll,   "enroll <name> <role>" },
    { "2fa",       2, c_2fa,      "2fa <on|off>" },
    { "passwd",    1, c_passwd,   "passwd <new_password>" },
    { "scan",      2, c_scan,     "scan <in|out> <uid>" },
    { "reboot",    2, c_reboot,   "reboot" },
    { NULL, 0, NULL, NULL }
};

static void c_help(int ac, char **av)
{
    int lvl = auth_level();
    printf("\n  Commands (your level: %d)\n", lvl);
    for (const cmd_t *c = CMDS; c->name; c++)
        printf("  %-34s %s%s\n", c->help, c->lvl > lvl ? "[needs level " : "", c->lvl > lvl ? (c->lvl == 1 ? "1] " : "2] ") : "");
    printf("\n");
}
static void run_line(char *line)
{
    char *av[8]; int ac = 0, in = 0;
    for (char *p = line; *p && ac < 8; p++) {                       /* split on spaces */
        if (*p == ' ' || *p == '\t') { *p = 0; in = 0; }
        else if (!in) { av[ac++] = p; in = 1; }
    }
    if (!ac) return;
    for (const cmd_t *c = CMDS; c->name; c++) {
        if (strcmp(av[0], c->name)) continue;
        if (auth_level() < c->lvl) { printf("Permission denied: login as %s first (type: login <user> <password>)\n", c->lvl == 2 ? "admin" : "manager/admin"); return; }
        c->fn(ac, av);
        return;
    }
    printf("Unknown command '%s'. Type help.\n", av[0]);
}

static void cli_task(void *arg)
{
    char line[96]; int n = 0;
    printf("\n=== CoCIS Smart Control - type 'help' ===\ncocis> ");
    fflush(stdout);
    for (;;) {
        uint8_t ch;
        if (uart_read_bytes(UART_NUM_0, &ch, 1, pdMS_TO_TICKS(200)) != 1) continue;
        if (ch == '\r' || ch == '\n') {
            line[n] = 0; printf("\n");
            if (n) run_line(line);
            n = 0;
            printf("cocis%s> ", auth_level() ? "#" : ""); fflush(stdout);
        } else if ((ch == 0x08 || ch == 0x7F) && n > 0) {
            n--; printf("\b \b"); fflush(stdout);
        } else if (ch >= 32 && ch < 127 && n < (int)sizeof line - 1) {
            line[n++] = ch; putchar(ch); fflush(stdout);
        }
    }
}

void cli_start(void)
{
    uart_driver_install(UART_NUM_0, 512, 0, 0, NULL, 0);
    xTaskCreate(cli_task, "cli", 6144, NULL, 2, NULL);
}
