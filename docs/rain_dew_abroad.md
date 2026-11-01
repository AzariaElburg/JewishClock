# Rain/dew prayer slide outside Israel - what it does, and the known gap

Written 2026-11-01. The clock is built for Israel; outside Israel the rain/dew
slide works, but has one known gap (below). Israel or abroad comes from the
clock's place (`Location.cpp`, `isIsrael()`); the wording from the nusach
setting (Ashkenaz / Sefard - Chassidim and Edot Hamizrach say what Sefard says).

## What is said, per switch point (in the Amidah)

G = second blessing (גבורות), B = ninth blessing (ברכת השנים, weekdays only).

| Switch point | G Ashkenaz abroad | G Sefard abroad | B Ashkenaz abroad | B Sefard abroad |
|---|---|---|---|---|
| Musaf, 22 Tishrei | משיב הרוח ומוריד הגשם | משיב הרוח ומוריד הגשם | - | - |
| Maariv of 4 Dec (5 Dec before a Gregorian leap year) | - | - | ותן טל ומטר לברכה | ברך עלינו |
| Musaf, 15 Nisan | **nothing** (stop משיב הרוח) | מוריד הטל | ותן ברכה | ברכנו |

In Israel everyone says מוריד הטל in summer, and B switches at Maariv of 7 Cheshvan.

## What the clock shows abroad

Everything that *is* said, with its when ("ממוסף", "מהערב", ...), the same as in
Israel. **The gap:** for Ashkenaz abroad at Pesach, G changes to saying nothing,
and the slide shows nothing about it - only "ותן ברכה ממוסף". The one change
that's easy to forget there (stop saying משיב הרוח) gets no reminder.

## To improve abroad support

Decisions:
- What the slide says for that gap, e.g. "בלי משיב הרוח, ותן ברכה ממוסף", or
  leave it as it is.
- Whether abroad needs more nusach choices than Ashkenaz / Sefard.

Points to check:
- B's start abroad: `Calendar::getRainRequest()` computes it (Julian 21/22
  November, the gap grows a day in 2100, 2200, 2300). Checked in the simulator
  for 2026 and 2027 only; never on a clock outside Israel.
- Second day of Yom Tov abroad: the slide counts days from 15 Nisan /
  22 Tishrei, as in Israel; B is first said at the next weekday Amidah
  (Chol Hamoed abroad) - the slide doesn't name that day.
- Code: `Calendar::getPrayerSwitch()` (G, incl. the Ashkenaz-abroad case),
  `Calendar::getRainRequest()` (B), the slide in `MessageAggregator.cpp`
  (`ContentType::PrayerSwitch`). Tests: `sim/test_prayer.py` (abroad cases).
