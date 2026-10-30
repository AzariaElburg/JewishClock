#pragma once
// Force-included into every simulator file (zig c++ -include). Routes the
// time-zone conversions the clock's code uses to the simulator's own exact
// POSIX-TZ implementation (sim_platform.cpp): the PC's C library can't apply
// a POSIX TZ string like the clock's "IST-2IDT,M3.4.4/26,M10.5.0".
#include <ctime>
struct tm* sim_localtime_r(const time_t* t, struct tm* out);
struct tm* sim_gmtime_r(const time_t* t, struct tm* out);
time_t sim_timegm(struct tm* tm);
#define localtime_r sim_localtime_r
#define gmtime_r sim_gmtime_r
// The ESP32's system clock: counts from 1970 at power on until the first
// time sync sets it (sim_modules.cpp) - not the PC's clock.
time_t sim_time(time_t* out);
#define time(p) sim_time(p)

// Newlib/ESP-IDF functions the PC's C library (MinGW) lacks.
#include <cstring>
#include <cstddef>
inline size_t strlcpy(char* dst, const char* src, size_t size) {
    size_t n = strlen(src);
    if (size) { size_t k = n < size - 1 ? n : size - 1; memcpy(dst, src, k); dst[k] = 0; }
    return n;
}
// setenv("TZ", ...) + tzset() is how the clock applies a time zone; in the
// simulator that goes to its own POSIX-TZ implementation.
int sim_setenv(const char* name, const char* value, int overwrite);
#define setenv sim_setenv
#define tzset() ((void)0)
