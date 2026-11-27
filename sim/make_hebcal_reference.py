"""
Fetches the reference answers sim/test_own_calculations_vs_hebcal.py checks the clock's own
calculations against - run once, the result is kept in git
(sim/hebcal_reference.json); again only to add years or places. ~60 requests.

- Hebrew dates: for each Hebrew year, the Gregorian date of the first day of every
  month, from Hebcal's own date of every day ("hebdate" items - what the clock
  downloads). Every month starting on the right day = every day of the year right.
- Sunset / tzeit7083deg (UTC minutes of the day): places from the equator to past
  the Arctic circle and both sides of the date line, every 5th day of a year plus
  the solstices, equinoxes and summer-time switches; None where Hebcal has none
  (the sun doesn't get that low).

    python sim/make_hebcal_reference.py            (fetch and keep - ~1 minute)
    python sim/make_hebcal_reference.py --check    (fetch and compare only; exit 1 if Hebcal changed -
                                                    the weekly GitHub monitor)
"""
import datetime
import json
import os
import sys
import time
import urllib.request
from pathlib import Path

OUT = Path(__file__).resolve().parent / "hebcal_reference.json"
MONTHS = {"Tishrei": 1, "Cheshvan": 2, "Kislev": 3, "Tevet": 4, "Sh'vat": 5, "Adar": 6, "Adar I": 6,
          "Nisan": 7, "Iyyar": 8, "Sivan": 9, "Tamuz": 10, "Av": 11, "Elul": 12, "Adar II": 13}
# The years the clock lives in, one with 2038, the 2100 non-leap century, the 6000 turn, its last year.
HEBREW_YEARS = list(range(5785, 5801)) + [5860, 5999, 6000, 6001, 6060]
PLACES = [  # name, lat, lon, tzid
    ("Jerusalem", 31.7767, 35.2345, "Asia/Jerusalem"),
    ("New York", 40.7128, -74.0060, "America/New_York"),
    ("Tromso", 69.6492, 18.9553, "Europe/Oslo"),            # midnight sun: no sunset, no tzeit
    ("London", 51.5074, -0.1278, "Europe/London"),          # summer: sunset but no tzeit (7.08 deg)
    ("Ushuaia", -54.8019, -68.3030, "America/Argentina/Ushuaia"),
    ("Auckland", -36.8485, 174.7633, "Pacific/Auckland"),    # far east of Greenwich
    ("Honolulu", 21.3069, -157.8583, "Pacific/Honolulu"),    # far west
    ("Quito", -0.1807, -78.4678, "America/Guayaquil"),       # equator
]
ZMANIM_YEAR = 2027
EXTRA_DAYS = ["03-20", "03-21", "03-26", "03-28", "06-20", "06-21", "06-22", "09-22", "09-23",
              "10-31", "11-07", "12-21", "12-22", "04-04", "09-26"]


def get(url):
    req = urllib.request.Request(url, headers={"User-Agent": "clock-reference-check"})
    for attempt in range(3):
        try:
            with urllib.request.urlopen(req, timeout=30) as r:
                return json.load(r)
        except Exception as e:
            print("  retry:", e)
            time.sleep(3)
    sys.exit("no answer: " + url)


def hebrew_years():
    out = {}
    for hy in HEBREW_YEARS:
        d = get("https://www.hebcal.com/hebcal?v=1&cfg=json&year=%d&yt=H&d=on&maj=off&min=off&mod=off&nx=off"
                "&ss=off&mf=off&c=off&s=off&o=off" % hy)
        starts = []
        for it in d["items"]:
            if it.get("category") != "hebdate":
                continue
            parts = it["hdate"].split(" ")
            day, year, month = int(parts[0]), int(parts[-1]), " ".join(parts[1:-1])
            if day == 1 and year == hy:
                starts.append([MONTHS[month], it["date"][:10]])
        out[str(hy)] = starts
        print("year", hy, len(starts), "months")
        time.sleep(0.5)
    return out


def utc_min(iso, day):
    t = datetime.datetime.fromisoformat(iso).astimezone(datetime.timezone.utc)
    return round((t - datetime.datetime.combine(day, datetime.time(), datetime.timezone.utc)).total_seconds() / 60)


def zmanim():
    days = set()
    d0 = datetime.date(ZMANIM_YEAR, 1, 1)
    for i in range(0, 365, 5):
        days.add(d0 + datetime.timedelta(days=i))
    days |= {datetime.date.fromisoformat("%d-%s" % (ZMANIM_YEAR, md)) for md in EXTRA_DAYS}
    out = []
    for name, lat, lon, tz in PLACES:
        times = {"sunset": {}, "tzeit7083deg": {}}
        for start, end in (("01-01", "04-30"), ("05-01", "08-31"), ("09-01", "12-31")):   # Hebcal: at most 180 days at once
            d = get("https://www.hebcal.com/zmanim?cfg=json&latitude=%.4f&longitude=%.4f&tzid=%s&start=%d-%s&end=%d-%s"
                    % (lat, lon, tz, ZMANIM_YEAR, start, ZMANIM_YEAR, end))
            for z in times:
                times[z].update(d["times"][z])
            time.sleep(0.5)
        rows = []
        for day in sorted(days):
            k = day.isoformat()
            if k not in times["sunset"] or k not in times["tzeit7083deg"]:
                sys.exit("Hebcal's answer has no entry for %s %s (a day missing, not a null)" % (name, k))
            ss, tz7 = times["sunset"][k], times["tzeit7083deg"][k]
            rows.append([k, utc_min(ss, day) if ss else None, utc_min(tz7, day) if tz7 else None])
        out.append({"place": name, "lat": lat, "lon": lon, "days": rows})
        print("place", name, len(rows), "days")
        time.sleep(0.5)
    return out


ANNIVERSARY_YEARS = 25   # each case's yahrzeit / Hebrew birthday this many years on


def anniversaries(years):
    """Hebcal's own yahrzeit and Hebrew birthday dates (its yahrzeit API - the rules the clock's
    JewishDateEngine ports) for the hard cases: 30 Cheshvan / 30 Kislev in a year whose next year
    has that day or not (a yahrzeit is fixed by the first anniversary), 30 Adar I, Adar I / Adar II
    in a leap year, Adar in a common year. The original dates from the month starts above (Hebcal's)."""
    starts = {int(y): {m: datetime.date.fromisoformat(g) for m, g in v} for y, v in years.items()}
    def length(y, m):   # days in month m of year y (the next month's start minus this one's)
        nxt = {2: 3, 3: 4}[m]
        return (starts[y][nxt] - starts[y][m]).days
    def leap(y):
        return 13 in starts[y]
    cases = []
    span = [y for y in range(5785, 5800) if y + 1 in starts]
    for m, name in ((2, "Cheshvan"), (3, "Kislev")):
        for next_long in (False, True):
            y = next((y for y in span if length(y, m) == 30 and (length(y + 1, m) == 30) == next_long), None)
            if y:
                cases.append(("30 %s %d (the next year's has %s)" % (name, y, "a 30th" if next_long else "no 30th"), starts[y][m] + datetime.timedelta(days=29)))
    ly = next(y for y in span if leap(y))
    cy = next(y for y in span if not leap(y))
    cases += [("30 Adar I %d" % ly, starts[ly][6] + datetime.timedelta(days=29)),
              ("15 Adar I %d" % ly, starts[ly][6] + datetime.timedelta(days=14)),
              ("15 Adar II %d" % ly, starts[ly][13] + datetime.timedelta(days=14)),
              ("15 Adar %d" % cy, starts[cy][6] + datetime.timedelta(days=14))]
    out = []
    for what, g in cases:
        for kind in ("Yahrzeit", "Birthday"):
            d = get("https://www.hebcal.com/yahrzeit?cfg=json&v=yahrzeit&years=%d&n1=X&t1=%s&d1=%d&m1=%d&y1=%d"
                    % (ANNIVERSARY_YEARS, kind, g.day, g.month, g.year))
            res = [[it["date"][:10], it["hdate"]] for it in d.get("items", []) if it.get("category") in ("yahrzeit", "birthday")]
            out.append({"case": what, "type": kind, "g": g.isoformat(), "dates": res})
            time.sleep(0.5)
        print("anniversary", what, g, len(res), "years")
    return out


def differences(kept, fresh):
    """What Hebcal answers now that differs from the kept reference - one line each."""
    out = []
    for hy in sorted(set(kept["hebrewYears"]) | set(fresh["hebrewYears"])):
        a, b = kept["hebrewYears"].get(hy), fresh["hebrewYears"].get(hy)
        if a != b:
            out.append("Hebrew year %s: month starts were %s, now %s" % (hy, a, b))
    kz = {(p["place"], d[0]): d[1:] for p in kept["zmanim"] for d in p["days"]}
    fz = {(p["place"], d[0]): d[1:] for p in fresh["zmanim"] for d in p["days"]}
    for k in sorted(set(kz) | set(fz)):
        if kz.get(k) != fz.get(k):
            out.append("%s %s: sunset/tzeit (UTC minutes) were %s, now %s" % (k[0], k[1], kz.get(k), fz.get(k)))
    ka = {(a["type"], a["g"]): a["dates"] for a in kept.get("anniversaries", [])}
    fa = {(a["type"], a["g"]): a["dates"] for a in fresh.get("anniversaries", [])}
    for k in sorted(set(ka) | set(fa)):
        # Hebcal lists from the current year on: only the years in both lists are compared (a new
        # January moves the list by a year - not a change of Hebcal's)
        a = {x[1].split()[-1]: x for x in ka.get(k, [])}
        b = {x[1].split()[-1]: x for x in fa.get(k, [])}
        diff = [(a[y], b[y]) for y in sorted(set(a) & set(b)) if a[y] != b[y]]
        if diff or (k in ka) != (k in fa):
            out.append("%s of %s: %s" % (k[0], k[1], "was %s, now %s" % diff[0] if diff else "a case only in one of the two"))
    return out


def main():
    t0 = time.time()
    years = hebrew_years()
    fresh = {"made": datetime.date.today().isoformat(), "hebrewYears": years, "zmanim": zmanim(),
             "anniversaries": anniversaries(years)}
    took = time.time() - t0
    if "--check" in sys.argv:   # the weekly monitor (.github/workflows/hebcal-reference-monitor.yml): compare, change nothing
        kept = json.loads(OUT.read_text(encoding="utf-8"))
        diff = differences(kept, fresh)
        print("Hebcal asked in %.0f s; kept reference made %s" % (took, kept.get("made")))
        if not diff:
            print("Hebcal answers as in the kept reference - nothing changed")
            return
        print("HEBCAL CHANGED: %d answer(s) differ from the kept reference:" % len(diff))
        if os.environ.get("GITHUB_ACTIONS"):   # an error note: GitHub's failure email shows these
            print("::error title=Hebcal changed::%d answer(s) differ from the kept reference, e.g. %s" % (len(diff), diff[0]))
        for line in diff[:60]:
            print("  " + line)
        print("Look at what changed; if Hebcal is right: python sim/make_hebcal_reference.py, then "
              "python sim/test_own_calculations_vs_hebcal.py - does our own calculation still agree?")
        sys.exit(1)
    OUT.write_text(json.dumps(fresh, separators=(",", ":")), encoding="utf-8")
    print("wrote", OUT, OUT.stat().st_size, "bytes, Hebcal asked in %.0f s" % took)


if __name__ == "__main__":
    main()
