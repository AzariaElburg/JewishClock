# JewishClock

A Hebrew calendar clock: an ESP32-S3 with a wide 480 x 128 colour display that shows, in
turn, the Hebrew and the civil date, Shabbat and holidays, the weekly parasha, candle-lighting
and Havdalah times, fasts, the Omer count, Daf Yomi and Mishnah Yomit, the rain/dew prayer
switches, and the family's own birthdays, anniversaries and yahrzeits - with pictures.

It gets its calendar from [Hebcal](https://www.hebcal.com), its time from NTP and its place
from the Wi-Fi network it is on, so it does not run out of dates. Everything is set from a web
page the clock serves itself - over the home Wi-Fi or over the clock's own Wi-Fi network.

Built as a gift: a replacement for a 35-year-old clock whose built-in date tables ended in 2025.

## Parts

| Part | Model | Notes |
|---|---|---|
| Microcontroller board | ESP32-S3-DevKitC-1, **N16R8** (16 MB flash, 8 MB PSRAM), CP2102 USB chip | the PSRAM holds pictures and the event list |
| Display | 3.9" IPS bar, 480 x 128, NV3041A controller, SPI - module **UE039HV-RB40-L009A** by VIEWE | datasheets in `docs/hardware/` |
| Display breakout | 40-pin, 0.5 mm pitch FPC/FFC adapter board | get one with the header pins already soldered |
| Light sensor | **GY-302** (BH1750) | optional - the brightness can be fixed instead |
| Backlight switch | "15A 400W" dual-MOSFET trigger module (terminals VIN+/VIN-/OUT+/OUT-, TRIG/PWM/GND) | a single LR7843 module switched only half on with 3.3 V and flickered |
| Backlight resistor | 12 Ω, 1 W | in series with the backlight LEDs |
| Power | 5 V USB supply | |

Wiring, connector by connector: `docs/hardware/ESP32_S3_Clock_Master_Wiring_Matrix_Pinout.docx`.
Every pin the firmware uses is defined in one place: `include/Config.h`.

## Building

The firmware builds with the community **pioarduino** platform (Arduino-ESP32 3.x on ESP-IDF 5.5 -
needed for 64-bit time), in its own PlatformIO installation:

    pio run -c pioarduino.ini -t upload      # firmware
    pio run -c pioarduino.ini -t uploadfs    # the web page and pictures (replaces the clock's file system)

The project file is `pioarduino.ini`, not `platformio.ini`. Details, and everything learned on the
way: `CLAUDE.md`.

## Simulator

The whole clock program also runs on a Windows PC, with stand-ins for the hardware only: real
time servers, real Hebcal, the web page in a browser, updates with their trial and rollback. All
tests run there: `python sim/run_all_tests.py`. See `sim/README.md`.

## Updates

New versions come as one signed file (`.jcupdate`: firmware plus web page), installed from the
clock's own page; the clock checks the signature with the public key in its firmware
(`include/UpdateKey.h`) and installs nothing else. A new version runs on trial and the clock goes
back to the previous one by itself if it fails.

## Questions and comments

Please open an Issue on this repository.

## Licence

© Azaria Elburg. All rights reserved. The licence is yet to be decided. Until then, nobody may use, copy,
change or redistribute this without written permission from Azaria. Third-party components and their licences: `THIRD_PARTY_LICENSES/`.
