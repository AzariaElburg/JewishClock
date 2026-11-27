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
| Musaf, 15 Nisan | בלי משיב הרוח | מוריד הטל | - | - |
| Maariv after the 2nd day of Yom Tov (16 Nisan; on a Friday: Motzei Shabbat) | - | - | ותן ברכה | ברכנו |

In Israel everyone says מוריד הטל in summer, B switches at Maariv of 7 Cheshvan
and at Maariv after the (one) first day of Pesach. B is never said on Shabbat
or Yom Tov, so an evening that starts Shabbat moves to Motzei Shabbat
(abroad, 4/5 December can be a Friday).

## What the clock shows abroad

Everything that *is* said, with its when ("ממוסף", "מהערב", ...), the same as in
Israel. For Ashkenaz abroad at Pesach, the slide shows "בלי משיב הרוח ממוסף" so
the change (stop saying משיב הרוח) gets its reminder (fixed 2026-11-21).

## To improve abroad support

Decisions:
- Whether abroad needs more nusach choices than Ashkenaz / Sefard.

Points to check:
- B's start abroad: `Calendar::getRainRequest()` computes it (Julian 21/22
  November, the gap grows a day in 2100, 2200, 2300). Checked in the simulator
  for 2026 and 2027 only; never on a clock outside Israel.
- Second day of Yom Tov abroad: G counts days from 15 Nisan / 22 Tishrei,
  as in Israel; B from the evening after 16 Nisan (Motzei Shabbat when that
  is a Friday) - both cases in `sim/test_prayer.py`, never seen on a clock
  outside Israel.
- Code: `Calendar::getPrayerSwitch()` (G, incl. the Ashkenaz-abroad "בלי משיב הרוח"),
  `Calendar::getRainRequest()` (B), the slide in `MessageAggregator.cpp`
  (`ContentType::PrayerSwitch`). Tests: `sim/test_prayer.py` (abroad cases).
