#pragma once
#include "app_config.h"
typedef enum { DOOR_IN = 0, DOOR_OUT = 1 } door_id_t;
typedef enum { ACT_GRANT, ACT_DENY, ACT_ALARM } door_act_t;
void door_init(void);
void door_act(door_id_t d, door_act_t a);   /* non-blocking: puts a command in the door's queue */
void door_beep(int times);                  /* short beeps, e.g. for alerts */
