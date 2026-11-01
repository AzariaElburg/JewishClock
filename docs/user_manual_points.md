# Points for the user manual and the one-page start guide

Collected since 2026-10-31; style, format and language are decided closer to
release (v2.1.0). Add a point whenever a feature, behavior or pitfall comes
up that a user would need to know. Not the manual itself - raw material.

Requirements (2026-10-31): both documents are served by the clock's own
settings page and travel in every update package, and each later release
(bug fixes, features - emailed, installed over Wi-Fi) brings them up to date.

## Legal / credits (for the manual's "about" part)
- Third-party licenses: Settings -> Clock software -> Licenses (/licenses.txt).
- Holiday and times data from Hebcal.com (CC BY 4.0); place search from Open-Meteo (CC BY 4.0, GeoNames).

## One-page start guide
- Plug in; first start shows the clock's own Wi-Fi name and password on its screen.
- Join that Wi-Fi with a phone/laptop, open 192.168.4.1 (or the page opens by itself).
- Enter the home Wi-Fi name and password; the clock restarts and joins it.
- Afterwards the settings page is at `jewishclock.local` or the address the
  clock shows; if neither works, the clock's own Wi-Fi always works.
- The place (for candle lighting etc.) is found from the network; check it in Settings.
- Add the family's events (Settings -> events, or Excel).
- Where to get help / what to send when something's wrong (the log).

## User manual - topics so far
**Screen**
- What the clock shows: Hebrew and Gregorian date, parasha, holidays, fasts,
  candle lighting / havdalah, Omer, rain/dew prayer switch, Rosh Chodesh,
  special Shabbatot, personal events with age / years.
- Display modes: alternating (one item at a time) vs single scroll; speed.
- Calendar content: which items show, their order, how often (frequency).
- The day changes at צאת הכוכבים (as printed calendars do) - Chanukah too.
- Rain/dew in prayer, two separate switches, one slide: משיב הרוח (from Musaf
  of 22 Tishrei and 15 Nisan - "ממוסף", the evening before "ממוסף מחר") and
  the request ותן טל ומטר (from Maariv of 7 Cheshvan in Israel, 4/5 December
  abroad - shown as "from tonight"). Nusach Ashkenaz or Sefard (ברך עלינו /
  ברכנו); days shown before and after; nothing before Pesach. Israel or abroad
  follows the place; Ashkenazim abroad get no מוריד הטל (they say nothing) -
  and no reminder to stop משיב הרוח (known gap, docs/rain_dew_abroad.md).
- Brightness: fixed or automatic (light sensor); night/day levels; learning the room.

**Wi-Fi and place**
- Up to three known networks; it joins the last one, else the strongest.
- "Connect" moves it to a network on purpose; "Remove" forgets one.
- Each network has its own place (home, parents', spare); the active place
  follows the network.
- Router that isolates devices: use the clock's own Wi-Fi to reach it.

**Events**
- Types: birthday, anniversary, yahrzeit, other; Hebrew and/or Gregorian date;
  "after sunset" for Hebrew dates; advance warning days.
- An event with both dates shows on each of them; a small Star of David after
  the age means "today / soon is its Hebrew date" (no mark = its Gregorian date).
  The age is counted in that calendar.
- An event on a Hebrew date shows from the evening before (צאת הכוכבים, when
  the Hebrew date changes), or only from midnight if chosen (Events row). Only
  the event follows this choice - the Hebrew date itself always changes at
  צאת. Gregorian dates: midnight.
- Icons and pictures (incl. short animations from a video); one shared picture library.
- Download all events as Excel, edit, upload - with a preview of what changes.

**Pictures**
- The clock's own pictures can be replaced with the family's; reverting.

**Safety and recovery**
- Backup (settings + events + Wi-Fi + pictures via Excel) and Restore,
  also from the setup page.
- Factory reset: erases everything; a backup restores it.
- Optional password for the settings page.

**Updates**
- Updates arrive as a file (by email): Settings -> Firmware Update -> choose -> Update.
- The new version runs on trial: no Wi-Fi within 5 min or a crash -> it returns
  to the previous version by itself. The page ends with "Done!".
- If the settings page itself is broken: `/update-safe`.

**Technician / advanced (maybe a separate part)**
- Screen calibration (window position/size, tear lines) - after a new box.
- Diagnostic log: viewing, downloading, sending.
- Debug time (testing a date) - always set back to real time.
