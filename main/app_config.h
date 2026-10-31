/* All pin numbers and tunable constants of the ENTRANCE terminal in one place. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"

/* ---- SPI bus shared by the 2 RFID readers and the SD card ---- */
#define SPI_HOST_ID     SPI3_HOST
#define PIN_SPI_SCK     18
#define PIN_SPI_MOSI    23
#define PIN_SPI_MISO    19
#define PIN_RFID_RST    21      /* reset line shared by both readers */
#define PIN_RFID_CS_IN  5       /* reader on the ENTRY side          */
#define PIN_RFID_CS_OUT 4       /* reader on the EXIT side           */
#define PIN_SD_CS       13

/* ---- door mechanisms (servo = door lock), indicators, buzzer ---- */
#define PIN_SERVO_IN    25
#define PIN_SERVO_OUT   26
#define PIN_LED_IN_G    32
#define PIN_LED_IN_R    33
#define PIN_LED_OUT_G   27
#define PIN_LED_OUT_R   14
#define PIN_BUZZER      17
#define PIN_EMERGENCY   16      /* "break glass" emergency release button (pull-up, press = 0) */

#define DOOR_OPEN_MS    3000
#define SCAN_COOLDOWN_MS 3000   /* ignore the same card at the same reader for 3 s */
#define MAX_USERS       32

typedef enum { ROLE_STUDENT = 0, ROLE_STAFF = 1, ROLE_ADMIN = 2 } role_t;
typedef enum { READER_IN = 0, READER_OUT = 1 } reader_t;

typedef struct {
    char   uid[15];     /* hex text, e.g. "01020304" (4-byte) or "04112233445566" (7-byte) */
    char   name[24];
    role_t role;
    bool   active;      /* false = card blocked (lost/stolen) */
    bool   inside;      /* true  = currently inside the building */
} user_t;

/* ---- LECTURE ROOMS (index 0 = Room 1, index 1 = Room 2) ---- */
#define NUM_ROOMS       2
#define PIN_PIR_1       36      /* input-only pin (VP) */
#define PIN_PIR_2       39      /* input-only pin (VN) */
#define PIN_LAMP_1      22
#define PIN_LAMP_2      2
#define ADC_CH_ROOM_1   ADC_CHANNEL_6   /* GPIO34 */
#define ADC_CH_ROOM_2   ADC_CHANNEL_7   /* GPIO35 */
#define OCC_HOLD_MS     20000   /* room is vacant after this long without motion (use 5-10 min in real life) */
#define LAMP_WATTS      36      /* 2 x 18 W tubes: used for the energy estimate */
#define DARK_BELOW_RAW  2000    /* ADC value below which a room counts as "dark" */
#define LDR_BRIGHT_IS_HIGH 1    /* set 0 if your LDR reads LOW when bright */
