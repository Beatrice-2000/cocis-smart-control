#pragma once
#include <stdbool.h>
void  rooms_init(void);                          /* sensors, lamps, timers, interrupts and one task per room */
void  rooms_print(void);                         /* for the serial menu */
bool  room_set_mode(int room_1_or_2, const char *mode);   /* "auto" | "on" | "off" */
void  rooms_energy(float *wh_used, float *wh_saved);
