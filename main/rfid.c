/*
 * Minimal MFRC522 driver (ISO 14443-A, UID only) written for ESP-IDF.
 * Follows the same command sequence as the well-known Arduino MFRC522 library:
 *   REQA -> anti-collision -> SELECT (with cascade levels for 7-byte UIDs) -> HALT
 */
#include <string.h>
#include "rfid.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "rfid";

/* MFRC522 registers */
enum { CommandReg = 0x01, ComIrqReg = 0x04, ErrorReg = 0x06, FIFODataReg = 0x09, FIFOLevelReg = 0x0A,
       ControlReg = 0x0C, BitFramingReg = 0x0D, CollReg = 0x0E, ModeReg = 0x11, TxControlReg = 0x14,
       TxASKReg = 0x15, TModeReg = 0x2A, TPrescalerReg = 0x2B, TReloadRegH = 0x2C, TReloadRegL = 0x2D,
       VersionReg = 0x37 };
/* commands */
enum { CMD_IDLE = 0x00, CMD_TRANSCEIVE = 0x0C, CMD_SOFTRESET = 0x0F };

static uint8_t rd(rfid_t *r, uint8_t reg)
{
    uint8_t tx[2] = { (uint8_t)(0x80 | ((reg << 1) & 0x7E)), 0 }, rx[2] = { 0 };
    spi_transaction_t t = { .length = 16, .tx_buffer = tx, .rx_buffer = rx };
    spi_device_polling_transmit(r->dev, &t);
    return rx[1];
}
static void wr(rfid_t *r, uint8_t reg, uint8_t val)
{
    uint8_t tx[2] = { (uint8_t)((reg << 1) & 0x7E), val };
    spi_transaction_t t = { .length = 16, .tx_buffer = tx };
    spi_device_polling_transmit(r->dev, &t);
}
static void set_mask(rfid_t *r, uint8_t reg, uint8_t m) { wr(r, reg, rd(r, reg) | m); }
static void clr_mask(rfid_t *r, uint8_t reg, uint8_t m) { wr(r, reg, rd(r, reg) & (uint8_t)~m); }

/* CRC_A of ISO 14443-3, calculated in software */
static void crc_a(const uint8_t *d, int n, uint8_t *out)
{
    uint16_t crc = 0x6363;
    for (int i = 0; i < n; i++) {
        uint8_t b = d[i];
        b ^= (uint8_t)(crc & 0xFF);
        b ^= (uint8_t)(b << 4);
        crc = (crc >> 8) ^ ((uint16_t)b << 8) ^ ((uint16_t)b << 3) ^ ((uint16_t)b >> 4);
    }
    out[0] = crc & 0xFF; out[1] = crc >> 8;
}

/* Send bytes to the card and collect the answer. *bits = valid bits in the last byte. */
static esp_err_t transceive(rfid_t *r, const uint8_t *send, uint8_t slen,
                            uint8_t *recv, uint8_t *rlen, uint8_t *bits)
{
    uint8_t txlast = bits ? *bits : 0;
    wr(r, CommandReg, CMD_IDLE);
    wr(r, ComIrqReg, 0x7F);                 /* clear all interrupt flags */
    set_mask(r, FIFOLevelReg, 0x80);        /* flush FIFO */
    for (int i = 0; i < slen; i++) wr(r, FIFODataReg, send[i]);
    wr(r, BitFramingReg, txlast);
    wr(r, CommandReg, CMD_TRANSCEIVE);
    set_mask(r, BitFramingReg, 0x80);       /* StartSend */

    bool done = false;
    int64_t t0 = esp_timer_get_time();
    for (int i = 0; esp_timer_get_time() - t0 < 40000; i++) {
        uint8_t n = rd(r, ComIrqReg);
        if (n & 0x30) { done = true; break; }       /* RxIRq | IdleIRq */
        if (n & 0x01) return ESP_ERR_TIMEOUT;       /* TimerIRq: nobody answered */
        if (i < 40) esp_rom_delay_us(50); else vTaskDelay(1);
    }
    clr_mask(r, BitFramingReg, 0x80);
    if (!done) return ESP_ERR_TIMEOUT;

    uint8_t err = rd(r, ErrorReg);
    if (err & 0x13) return ESP_FAIL;                /* buffer overflow / parity / protocol */
    if (err & 0x08) return ESP_ERR_INVALID_STATE;   /* collision */
    if (recv && rlen) {
        uint8_t n = rd(r, FIFOLevelReg);
        if (n > *rlen) return ESP_ERR_NO_MEM;
        *rlen = n;
        for (int i = 0; i < n; i++) recv[i] = rd(r, FIFODataReg);
        if (bits) *bits = rd(r, ControlReg) & 0x07;
    }
    return ESP_OK;
}

esp_err_t rfid_bus_init(void)
{
    gpio_config_t o = { .pin_bit_mask = (1ULL << PIN_RFID_RST) | (1ULL << PIN_SD_CS), .mode = GPIO_MODE_OUTPUT };
    gpio_config(&o);
    gpio_set_level(PIN_SD_CS, 1);
    gpio_set_level(PIN_RFID_RST, 0); vTaskDelay(pdMS_TO_TICKS(20));     /* hard reset */
    gpio_set_level(PIN_RFID_RST, 1); vTaskDelay(pdMS_TO_TICKS(60));

    spi_bus_config_t bus = { .mosi_io_num = PIN_SPI_MOSI, .miso_io_num = PIN_SPI_MISO, .sclk_io_num = PIN_SPI_SCK,
                             .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = 4000 };
    return spi_bus_initialize(SPI_HOST_ID, &bus, SPI_DMA_CH_AUTO);
}

esp_err_t rfid_init(rfid_t *r, int cs)
{
    r->cs = cs;
    spi_device_interface_config_t d = { .clock_speed_hz = 1000000, .mode = 0, .spics_io_num = cs, .queue_size = 2 };
    esp_err_t e = spi_bus_add_device(SPI_HOST_ID, &d, &r->dev);
    if (e != ESP_OK) return e;

    wr(r, CommandReg, CMD_SOFTRESET);
    vTaskDelay(pdMS_TO_TICKS(50));
    wr(r, TModeReg, 0x80);  wr(r, TPrescalerReg, 0xA9);     /* timer: ~25 ms timeout */
    wr(r, TReloadRegH, 0x03); wr(r, TReloadRegL, 0xE8);
    wr(r, TxASKReg, 0x40);  wr(r, ModeReg, 0x3D);
    if ((rd(r, TxControlReg) & 0x03) != 0x03) set_mask(r, TxControlReg, 0x03);   /* antenna on */
    uint8_t v = rd(r, VersionReg);
    ESP_LOGI(TAG, "reader CS=%d version=0x%02X %s", cs, v, (v == 0x00 || v == 0xFF) ? "(NOT RESPONDING - check wiring)" : "");
    return ESP_OK;
}

static bool reqa(rfid_t *r)
{
    clr_mask(r, CollReg, 0x80);
    uint8_t cmd = 0x26, atqa[2], alen = 2, bits = 7;
    return transceive(r, &cmd, 1, atqa, &alen, &bits) == ESP_OK && alen == 2;
}

static bool select_uid(rfid_t *r, uint8_t *uid, uint8_t *len)
{
    static const uint8_t SEL[3] = { 0x93, 0x95, 0x97 };      /* cascade levels 1..3 */
    uint8_t tmp[12]; int n = 0;
    for (int lvl = 0; lvl < 3; lvl++) {
        uint8_t buf[9], rb[5], rl = 5, bits = 0;
        clr_mask(r, CollReg, 0x80);
        buf[0] = SEL[lvl]; buf[1] = 0x20;                      /* anti-collision */
        if (transceive(r, buf, 2, rb, &rl, &bits) != ESP_OK || rl != 5) return false;
        if ((rb[0] ^ rb[1] ^ rb[2] ^ rb[3]) != rb[4]) return false;   /* BCC check */

        buf[1] = 0x70; memcpy(&buf[2], rb, 5);                 /* SELECT */
        uint8_t crc[2]; crc_a(buf, 7, crc); buf[7] = crc[0]; buf[8] = crc[1];
        uint8_t sak[3], sl = 3; bits = 0;
        if (transceive(r, buf, 9, sak, &sl, &bits) != ESP_OK || sl != 3) return false;

        bool cascade = (sak[0] & 0x04) != 0;                   /* UID not complete yet */
        if (cascade) { memcpy(&tmp[n], &rb[1], 3); n += 3; }   /* skip cascade tag 0x88 */
        else         { memcpy(&tmp[n], rb, 4);     n += 4; memcpy(uid, tmp, n); *len = n; return true; }
    }
    return false;
}

static void halt(rfid_t *r)
{
    uint8_t buf[4] = { 0x50, 0x00 }, crc[2];
    crc_a(buf, 2, crc); buf[2] = crc[0]; buf[3] = crc[1];
    transceive(r, buf, 4, NULL, NULL, NULL);                   /* the card never answers: timeout is normal */
}

bool rfid_read_uid(rfid_t *r, uint8_t *uid, uint8_t *len)
{
    if (!reqa(r)) return false;
    if (!select_uid(r, uid, len)) return false;
    halt(r);
    return true;
}
