"""
Every simulated clock's ports - web (home), its own Wi-Fi (ap), control - in
one place (2026-11-08). Each kind of clock has its own, so no test lands on
another clock: the drills used the work simulator's ports by default, wrote
into it and switched it off, and the Wi-Fi trips (which factory-reset their
clock) would have wiped it. --port-offset shifts a test clock's ports, for
two runs side by side.
"""
PORTS = {
    "family": (8070, 8071, 8079),   # sim/start_clock.py family: the parents' exact build
    "dev":    (8080, 8081, 8090),   # sim/start_clock.py dev: the working build
    "ui":     (8060, 8061, 8069),   # sim/ui_tests.py
    "drills": (8180, 8181, 8190),   # sim/drills.py
    "trips":  (8380, 8381, 8390),   # the Wi-Fi trips' clock (run_all_tests.py; utils/ap_tests.py --sim, sim/compare_web.py)
    "guide":  (8580, 8581, 8590),   # utils/make_install_guide.py: a clock at the family's version, updated for the screenshots
}


def ports(kind, offset=0):
    """(home, ap, control) of that kind of clock, shifted by offset."""
    return tuple(p + offset for p in PORTS[kind])


def urls(kind, offset=0):
    """http://127.0.0.1:<port> for (home, ap, control)."""
    return tuple("http://127.0.0.1:%d" % p for p in ports(kind, offset))
