#pragma once
// PC stand-in (jcclock). RTC_NOINIT_ATTR memory keeps its content through a
// restart, a crash or a watchdog reset, not through a power cut - as on the
// clock: sim_clock_main.cpp saves this section to a file when the simulated
// clock restarts and loads it back at its next start (not after a power cut).
// Section ".rtcni$b": the "$" groups it between the start/end markers in
// ".rtcni$a" / ".rtcni$c" (PE/COFF linkers sort grouped sections by suffix).
#define RTC_NOINIT_ATTR __attribute__((section(".rtcni$b")))
#define RTC_DATA_ATTR   __attribute__((section(".rtcni$b")))
#define IRAM_ATTR
#define DRAM_ATTR
