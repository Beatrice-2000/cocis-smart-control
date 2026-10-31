#pragma once
#include "app_config.h"

typedef enum { AR_GRANTED, AR_GRANTED_ANOMALY, AR_DENIED_UNKNOWN, AR_DENIED_BLOCKED,
               AR_DENIED_SCHEDULE, AR_DENIED_PASSBACK, AR_ENROLLED } ar_t;
const char *ar_str(ar_t r);
bool ar_granted(ar_t r);

void access_init(void);
ar_t access_process(reader_t rd, const char *uid, char *name_out, size_t nlen);
void uid_to_str(const uint8_t *uid, uint8_t len, char out[15]);

/* occupancy */
int  access_occupancy(void);
void access_print_inside(void);
void access_stats(int *in, int *out, int *denied, int *anomaly, int *peak);

/* users (all take the users mutex internally) */
int  user_add(const char *uid, const char *name, role_t role);          /* 0 ok, <0 error */
int  user_del(const char *uid);
int  user_set_active(const char *uid, bool active);
void users_print(void);
bool enroll_arm(const char *name, role_t role);                          /* next unknown card becomes a user */
const char *role_str(role_t r);
bool role_parse(const char *s, role_t *r);

/* schedule (minutes after midnight, local time) */
bool sched_is_open(void);
void sched_get(int *open_min, int *close_min);
bool sched_set(int open_min, int close_min);
bool sched_parse_hhmm(const char *s, int *minutes);

/* optional second factor: an ADMIN card must have been scanned recently */
bool access_admin_card_recent(int seconds);
bool cfg_get_2fa(void);
void cfg_set_2fa(bool on);
