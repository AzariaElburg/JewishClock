"""
The test clocks' time server (2026-11-21, the user's OK): every simulated clock of a test run asks
this one, on the PC, instead of pool.ntp.org & co - dozens of clocks asking from one home address got
it rate-limited by a pool server (silent drops, "RATE" kisses: the night's "outages"). It answers as a
time server does, with the PC's own time (Windows keeps it synced) - the simulator sends every time
question here when JC_NTP_RELAY is set, and hands the answer to the clock as coming from the address
it asked (sim/sim_net.cpp), as a network would. Not for every check: the boot drill and the family's
old version still ask the real servers, few and far between.

    from time_relay import ensure_relay; ensure_relay()   # once per run: starts it, sets JC_NTP_RELAY
"""
import os
import socket
import struct
import threading
import time

NTP_EPOCH = 2208988800


def _ts(t):
    sec = int(t) + NTP_EPOCH
    return struct.pack(">II", sec & 0xFFFFFFFF, int((t - int(t)) * 2 ** 32) & 0xFFFFFFFF)


def _serve(sock):
    while True:
        try:
            q, addr = sock.recvfrom(512)
        except OSError:
            return
        t_in = time.time()
        if len(q) < 48 or (q[0] & 7) != 3:   # a client's question only
            continue
        poll = q[2]
        head = struct.pack(">BBbb", (0 << 6) | (4 << 3) | 4, 2, poll, -20)   # LI 0, v4, server; stratum 2, 1 us steps
        root = struct.pack(">II", 0, int(0.001 * 65536))                    # root delay 0, dispersion 1 ms
        ref_id = socket.inet_aton("127.0.0.1")
        reply = head + root + ref_id + _ts(t_in - 16) + q[40:48] + _ts(t_in) + _ts(time.time())
        try:
            sock.sendto(reply, addr)
        except OSError:
            pass


def ensure_relay():
    """The relay of this run: started once (a thread of this process) unless one is already named by
    JC_NTP_RELAY (a parent process's - the lanes and drills inherit it). Returns its port."""
    if os.environ.get("JC_NTP_RELAY"):
        return int(os.environ["JC_NTP_RELAY"])
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    threading.Thread(target=_serve, args=(s,), daemon=True).start()
    os.environ["JC_NTP_RELAY"] = str(port)
    return port
