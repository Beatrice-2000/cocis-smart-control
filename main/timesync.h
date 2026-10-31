/* Time of day for the access schedule: Wi-Fi + SNTP (optional: the system also works with a manually set clock). */
#pragma once
#include <stdbool.h>
#include <time.h>
void ts_start(void);                 /* connect to Wi-Fi and start SNTP in the background */
bool ts_wifi_up(void);
bool time_valid(void);               /* true once the clock has been set (SNTP or manual) */
bool time_get_local(struct tm *out); /* local time (East Africa, UTC+3); returns time_valid() */
