# Midnight - every place that uses the date or the time of day (audit 2026-11-21)

The user, 2026-11-21: *"midnight is not to break anything. It should either be ignored or taken
into account - but not just left to chance."*

## The rule that covers all of them

**One moment per main-loop pass** (`NTPManager::beginPass()` / `endPass()`, called first and last in
`main.cpp` `loop()`; the screen simulator's `loopLikeTheClock()` the same). Every time read in between -
`getTime()`, `getDateTime()`, `getLocalEpoch()`, from any module, and from the web task while the pass
runs - is that one moment. So a new day (or hour, or minute) only ever comes **between** two passes,
never in the middle of one: the calendar's "today", the slides, the events' "days until", the date line
and the candle lighting are always about the same day. A pass takes milliseconds; the next one sees the
new day everywhere at once.

Before this, the calendar's step and the slides' step read the time separately, a few ms apart: a
midnight between them could show for up to a second a slide mixing yesterday's day with today's date.

## Place by place

| Place | What it does with the date/time | Across midnight |
|---|---|---|
| `Calendar::update()` - the fetch check | "is today in the saved calendar? is it long enough ahead?" | **Fixed:** used the UTC date, the rest of the clock the local one - from local midnight to 02:00/03:00 it asked about yesterday. Now the local date. A download asked before midnight covers today and the days ahead (14+), so its answer after midnight holds the new day too. |
| Calendar download (asynchronous) | asked with start = today, answered later | Taken into account: the answer is keyed by date, not by "when asked"; parsed the moment it arrives. Test: `sim/test_midnight.py` case 3 (asked 23:59, answered 00:01). |
| `Calendar::syncToday()` | which saved day is "today" - Hebrew date at צאת הכוכבים, civil day (candles/Havdalah) at midnight | Reads one moment (date and time from the same reading). Runs every pass. |
| Parasha / Mevarchim window (`Calendar.cpp`, Shabbat's end) | until Havdalah | Same moment as the rest of the pass. |
| Show windows (`nowLocalSeconds()`) | when a slide may be shown | Same moment. |
| `MessageAggregator` - slides rebuilt every second | date line, events "today / in N days", candles, status line | Same moment as the calendar in that pass; a date slide shows the new date within a second of 00:00. Test: `test_midnight.py` case 1 (both display modes) and case 2 (Friday night: no candle lighting after 00:00, Havdalah yes). |
| Events' "days until" (`getTodayAbsolute()`) | Gregorian today | Same moment. |
| Online update - daily check, night install hour | hour == AUTO_INSTALL_HOUR | An hour window, read once per call - midnight not involved (the hour is 03:00). |
| "On the screen for 3 days" counters (failed update, texts too long, update feed missing) | local day number | Count days by the local date: a new day at midnight by design. |
| Timed log level (`logLevelUntil`) | an end moment (seconds) | A moment, not a date - midnight irrelevant. |
| Web page answers (`/api/status`, debug time, convert) | today's date for the page | Same moment as the pass running then. |
| Diagnostic log time stamps | the time of each line | Lock-free reading (DebugTime.h), each line its own moment - a line at 23:59:59.9 and the next at 00:00:00.1 are simply two moments. Nothing derived from them. |
| NTP itself (a jump over midnight) | the time steps to another day | Drill `ntp`: "+1 day: the date is tomorrow's, the Hebrew date changed" and back. |

## Tests

- `sim/test_midnight.py` (display group of `run_all_tests.py`): a plain midnight in both display modes,
  Friday night into Shabbat, a calendar download asked before midnight and answered after it.
- Test scripts themselves never take "today" from the PC across a midnight: `web_ui.clear_of_midnight()`,
  the pictures drill holds its day (see CLAUDE.md, 2026-11-21).
