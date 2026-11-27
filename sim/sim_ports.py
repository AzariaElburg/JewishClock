"""
Every simulated clock's ports - web (home), its own Wi-Fi (ap), control - in
one place (2026-11-08). Each kind of clock has its own, so no test lands on
another clock: the drills (resilience) used the work simulator's ports by default, wrote
into it and switched it off, and the Wi-Fi trips (which factory-reset their
clock) would have wiped it. --port-offset shifts a test clock's ports, for
two runs side by side.
"""
PORTS = {
    "family": (8070, 8071, 8079),   # sim/start_clock.py family: the parents' exact build
    "dev":    (8080, 8081, 8090),   # sim/start_clock.py dev: the working build
    "web_ui": (8060, 8061, 8069),   # sim/web_ui.py
    "resilience": (8180, 8181, 8190),   # sim/resilience.py
    "trips":  (8380, 8381, 8390),   # utils/ap_tests.py --sim (the trips against a simulator by hand)
    "from_family": (8480, 8481, 8490),   # sim/web_ui.py from_family: a clock at the family's version, updated
    "simulator_validity": (8680, 8681, 8690),   # run_all_tests.py: the clock compare_web.py checks against the real clock's answers
    "guide":  (8580, 8581, 8590),   # utils/make_install_guide.py: a clock at the family's version, updated for the screenshots
    "manual": (8780, 8781, 8790),   # utils/manual_shots.py: a clock with made-up data, for the screenshots in Data/manual.html
}


def ports(kind, offset=0):
    """(home, ap, control) of that kind of clock, shifted by offset."""
    return tuple(p + offset for p in PORTS[kind])


def urls(kind, offset=0):
    """http://127.0.0.1:<port> for (home, ap, control)."""
    return tuple("http://127.0.0.1:%d" % p for p in ports(kind, offset))


# A test clock never asks the real update feed by itself (2026-11-16): every clock asks
# 3 minutes after its start (OnlineUpdate.cpp FIRST_CHECK_AFTER_MS), a full run starts dozens,
# and eight runs in a row used up GitHub's 60 questions an hour - the one test meant for the
# real feed then got 403. This address refuses at once ("Could not ask for updates", nothing
# on the screen); a test that wants a feed sets its own (config updateFeed, /api/config).
NO_FEED = "http://127.0.0.1:9/no-update-feed-in-tests"


def no_real_feed(fs):
    """Points the clock whose flash folder is fs at NO_FEED (its config.json, before it starts)."""
    import json
    from pathlib import Path
    f = Path(fs) / "config.json"
    cfg = json.loads(f.read_text(encoding="utf-8"))
    cfg["updateFeed"] = NO_FEED
    f.write_text(json.dumps(cfg, ensure_ascii=False, separators=(",", ":")), encoding="utf-8")
