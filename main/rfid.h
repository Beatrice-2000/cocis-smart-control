#pragma once
#include "app_config.h"
typedef struct { spi_device_handle_t dev; int cs; } rfid_t;
esp_err_t rfid_bus_init(void);                 /* SPI bus + hard reset of both readers */
esp_err_t rfid_init(rfid_t *r, int cs_pin);    /* configure one MFRC522 */
/* Returns true and fills uid[]/len when a card is present and was read. */
bool rfid_read_uid(rfid_t *r, uint8_t *uid, uint8_t *len);
