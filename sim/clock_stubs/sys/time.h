#pragma once
// PC stand-in (jcclock): the ESP32's system clock - counts from 1970 at power
// on until settimeofday() sets it (NTPManager.cpp, from the time server's
// answer); then it runs on with millis() (sim/sim_ntp.cpp). Windows' timeval
// holds seconds in 32 bits (the 2038 bug again) - here they are 64-bit.
#include_next <sys/time.h>
#include <cstdint>
struct sim_timeval { int64_t tv_sec; long tv_usec; };
#define timeval sim_timeval
#ifndef suseconds_t
#define suseconds_t long
#endif
int sim_settimeofday(const struct sim_timeval* tv, const void* tz);
int sim_gettimeofday(struct sim_timeval* tv, void* tz);
#define settimeofday sim_settimeofday
#define gettimeofday sim_gettimeofday
