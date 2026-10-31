idf_component_register(SRCS "main.c" "rfid.c" "door.c" "store.c" "access.c" "auth.c" "cli.c" "rooms.c" "timesync.c"
                       INCLUDE_DIRS "."
                       REQUIRES driver esp_timer nvs_flash mbedtls fatfs sdmmc esp_adc
                                esp_driver_sdspi esp_driver_spi esp_driver_uart
                                esp_wifi esp_event esp_netif lwip)
