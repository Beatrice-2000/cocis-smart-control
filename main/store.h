#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

bool store_init(void);                      /* mount SD card, start the logger task. Returns SD status */
bool store_sd_ok(void);
void store_lock(void);                      /* the SD card is shared: always lock before fopen/fclose */
void store_unlock(void);

void log_access(const char *fmt, ...) __attribute__((format(printf, 1, 2)));   /* -> ACCESS.CSV  */
void log_sys(const char *fmt, ...)    __attribute__((format(printf, 1, 2)));   /* -> SYSTEM.LOG  */
int  log_recent(char out[][160], int max);  /* copy the newest access-log lines from RAM */
void store_dump(const char *file);          /* print a file from the SD card to the serial console */
void fmt_timestamp(char *buf, size_t n);
