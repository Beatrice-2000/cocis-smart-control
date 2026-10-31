#pragma once
#include <stdbool.h>
/* functions implemented in main.c and used by the serial interface */
void status_print(void);
void cli_start(void);
bool scan_inject(int reader, const char *uid);
