# CoCIS Smart Control and Monitoring System: ONE ESP32 (ESP-IDF + Wokwi)

One ESP32 runs everything, organised as three logical terminals:

| Logical terminal | What it controls | Main files |
|---|---|---|
| Entrance | 2 RFID readers, 2 servo door locks, LEDs, buzzer, SD card, emergency button, serial menu | `main/main.c`, `rfid.c`, `door.c`, `access.c`, `store.c`, `auth.c`, `cli.c` |
| Room 1 and Room 2 | PIR motion, LDR light, lamp | `main/rooms.c` |
| Time | Wi-Fi + SNTP clock (only use of the network) | `main/timesync.c` |

## Build
```
idf.py set-target esp32
idf.py build
```
Built and checked with ESP-IDF **v5.4.2**.

## Run in Wokwi
Open this folder in VS Code (Wokwi Simulator extension + free licence) and run **Wokwi: Start Simulator**.
In the serial monitor type `help`.

Default demo accounts (change them with `passwd`): `admin / cocis2026`, `manager / manager2026`.

## Demo cards (Wokwi MFRC522 presets)
Blue 01:02:03:04 (ADMIN) | Green 11:22:33:44 (STAFF) | Yellow 55:66:77:88 (STUDENT) |
NFC tag 04:11:22:33:44:55:66 (STUDENT) | Key fob C0:FF:EE:99 (STAFF) | Red AA:BB:CC:DD (NOT registered).
Users are loaded from `sdcard/users.csv`.

## Pins
SPI 18/23/19, RFID CS 5 (entry) and 4 (exit), RFID reset 21, SD CS 13, servos 25/26,
LEDs 32/33/27/14, buzzer 17, emergency button 16.
Room 1: PIR 36 (VP), LDR 34, lamp 22.  Room 2: PIR 39 (VN), LDR 35, lamp 2.

## If the virtual RFID reader misbehaves
`login admin cocis2026` then `scan in 01020304` / `scan out 01020304` (logged as virtual taps).
