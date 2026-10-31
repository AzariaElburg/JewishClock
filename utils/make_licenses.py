"""Builds Data/licenses.txt: every third-party part of the clock, with its license.

The clock serves it (/licenses.txt, linked from Settings > Advanced) and it
rides along in every update package, so the notices always travel with the
firmware that contains the code - what the BSD/MIT/Apache/LGPL licenses ask
of anyone handing out the program (2026-10-31).

    python utils/make_licenses.py

Run it after adding, removing or upgrading a library, the framework, the
font, or an online service. Library versions are read from pioarduino.ini.
The license texts themselves are kept in THIRD_PARTY_LICENSES/ (exact copies
from each project or from the framework's own source headers).
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LIC = ROOT / "THIRD_PARTY_LICENSES"
OUT = ROOT / "Data" / "licenses.txt"

# Each part: (name, what it does here, license, copyright line, home, license file).
# A license file shared by several parts is printed once, at the end.
PARTS = [
    ("Arduino core for ESP32 (arduino-esp32) {framework}", "the framework the firmware is built on",
     "LGPL-2.1-or-later", "Copyright (c) Espressif Systems and the Arduino team",
     "https://github.com/espressif/arduino-esp32", "LGPL-2.1.txt"),
    ("ESP-IDF", "Espressif's system software inside the framework (Wi-Fi, TLS, storage)",
     "Apache-2.0 (its components carry their own notices, below and in ESP-IDF)",
     "Copyright (c) Espressif Systems (Shanghai) Co. Ltd.",
     "https://github.com/espressif/esp-idf", "Apache-2.0.txt"),
    ("lwIP", "TCP/IP networking, part of ESP-IDF", "BSD-3-Clause",
     "Copyright (c) 2001-2004 Swedish Institute of Computer Science",
     "https://savannah.nongnu.org/projects/lwip/", "lwIP-BSD.txt"),
    ("FreeRTOS kernel", "task scheduling, part of ESP-IDF", "MIT",
     "Copyright (C) 2021 Amazon.com, Inc. or its affiliates",
     "https://www.freertos.org", "FreeRTOS-MIT.txt"),
    ("Mbed TLS", "secure (https) downloads, part of ESP-IDF", "Apache-2.0 (dual: Apache-2.0 OR GPL-2.0-or-later; used under Apache-2.0)",
     "Copyright The Mbed TLS Contributors",
     "https://github.com/Mbed-TLS/mbedtls", "Apache-2.0.txt"),
    ("ESPAsyncWebServer {ESPAsyncWebServer}", "the settings page's web server", "LGPL-3.0-or-later",
     "Copyright 2016-2026 Hristo Gochkov, Mathieu Carbou, Emil Muratov, Will Miles",
     "https://github.com/ESP32Async/ESPAsyncWebServer", "LGPL-3.0.txt"),
    ("AsyncTCP {AsyncTCP}", "network connections for the web server", "LGPL-3.0-or-later",
     "Copyright 2016-2026 Hristo Gochkov, Mathieu Carbou, Emil Muratov, Will Miles",
     "https://github.com/ESP32Async/AsyncTCP", "LGPL-3.0.txt"),
    ("ArduinoJson {ArduinoJson}", "reading and writing settings and downloads", "MIT",
     "Copyright (c) 2014-2026 Benoit BLANCHON", "https://arduinojson.org", "ArduinoJson-MIT.txt"),
    ("GFX Library for Arduino {GFX Library for Arduino}", "the display driver", "BSD (Adafruit GFX license)",
     "Copyright (c) 2012 Adafruit Industries; Arduino_GFX by Moon On Our Nation",
     "https://github.com/moononournation/Arduino_GFX", "Arduino_GFX-BSD.txt"),
    ("BH1750 {BH1750}", "the light sensor", "MIT", "Copyright (c) 2018 claws",
     "https://github.com/claws/BH1750", "BH1750-MIT.txt"),
    ("SortableJS 1.15.2", "drag-to-reorder lists on the settings page (Data/sortable.min.js)", "MIT",
     "Copyright (c) 2019 All contributors to Sortable",
     "https://github.com/SortableJS/Sortable", "SortableJS-MIT.txt"),
    ("Noto Sans Hebrew", "the clock's font, converted to its pixel format", "SIL Open Font License 1.1",
     "Copyright 2022 The Noto Project Authors",
     "https://github.com/notofonts/hebrew", "NotoSansHebrew-OFL.txt"),
]

# Data the clock downloads while running (not part of the program itself).
SERVICES = [
    ("Hebcal.com", "holidays, Torah readings, candle-lighting and other times",
     "Data from Hebcal.com, licensed CC BY 4.0 (https://creativecommons.org/licenses/by/4.0/). "
     "The clock shows it reformatted. https://www.hebcal.com"),
    ("Open-Meteo geocoding", "finding a place's coordinates and time zone by name",
     "Data from Open-Meteo.com, licensed CC BY 4.0 (https://creativecommons.org/licenses/by/4.0/), "
     "based on GeoNames (CC BY 4.0). Free use is for non-commercial purposes. https://open-meteo.com"),
    ("ip-api.com", "guessing the place from the internet connection",
     "Free use is for non-commercial purposes. https://ip-api.com"),
    ("NTP Pool and public time servers", "the time of day", "No license terms for clients. https://www.ntppool.org"),
    ("Time-zone rules (Data/tz.csv)", "time zones outside Israel",
     "Derived from the IANA time zone database (public domain) via ICU. https://www.iana.org/time-zones"),
]

# The LGPL texts refer to the GPL; it has to travel with them.
EXTRA = [("GPL-3.0.txt", "GNU General Public License 3.0 (referred to by the LGPL 3.0)")]


def versions():
    ini = (ROOT / "pioarduino.ini").read_text(encoding="utf-8")
    v = {name.strip(): ver.strip() for name, ver in re.findall(r"^\s+[\w.-]+/([^@\n]+?)\s*@\s*([\d.]+)", ini, re.M)}
    # The framework version the build really used (PLATFORMIO_CORE_DIR, CLAUDE.md).
    fw = Path("C:/pio3/packages/framework-arduinoespressif32/package.json")
    m = re.search(r'"version":\s*"([^"]+)"', fw.read_text(encoding="utf-8")) if fw.exists() else None
    if not m:
        raise SystemExit(f"framework version not found in {fw} - build once first")
    v["framework"] = m.group(1)
    return v


def main():
    v = versions()
    out = ["JewishClock - third-party software and data",
           "=" * 44, "",
           "The clock's own code is its authors'. It is built with, and contains,",
           "the following parts made by others, each under its own license.",
           "Full license texts follow the list.", ""]
    for name, role, lic, copy, home, _ in PARTS:
        name = re.sub(r"\{([^}]+)\}", lambda m: v.get(m.group(1), ""), name).strip()
        out += ["* " + name, "  " + role, "  License: " + lic, "  " + copy, "  " + home, ""]
    out += ["Online data the clock uses", "-" * 26, ""]
    for name, role, terms in SERVICES:
        out += ["* " + name + " - " + role, "  " + terms, ""]
    seen = []
    for *_, f in PARTS:
        if f not in seen:
            seen.append(f)
    texts = [(f, f.rsplit(".", 1)[0]) for f in seen] + EXTRA
    for f, title in texts:
        out += ["", "=" * 72, title, "=" * 72, "", (LIC / f).read_text(encoding="utf-8").rstrip(), ""]
    OUT.write_text("\n".join(out) + "\n", encoding="utf-8", newline="\n")
    print(f"{OUT.relative_to(ROOT)}: {OUT.stat().st_size:,} bytes, {len(PARTS)} parts, {len(texts)} license texts")


if __name__ == "__main__":
    main()
