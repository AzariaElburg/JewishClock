#pragma once
// PC stand-in for the Arduino core (simulator only - see sim/README.md).
// Time comes from the simulator's own clock (sim_platform.cpp), not the PC's.

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <cmath>
#include <cctype>
#include <ctime>
#include <type_traits>
#include "WString.h"

typedef uint8_t byte;
typedef bool boolean;

#define PROGMEM
#define pgm_read_byte(p) (*(const uint8_t*)(p))
#define pgm_read_word(p) (*(const uint16_t*)(p))
#define pgm_read_dword(p) (*(const uint32_t*)(p))
#define F(s) (s)
#define IRAM_ATTR
// Pins: nothing wired in the simulator.
#define INPUT 0x01
#define INPUT_PULLDOWN 0x09
#define RISING 0x01
inline void pinMode(uint8_t, uint8_t) {}
inline void attachInterrupt(uint8_t, void (*)(), int) {}
#define HEX 16
#define DEC 10

// Simulated time (sim_platform.cpp).
unsigned long millis();
unsigned long micros();
void delay(unsigned long ms);
inline void yield() {}

template <typename A, typename B> inline auto min(A a, B b) -> typename std::common_type<A, B>::type { return a < b ? a : b; }
template <typename A, typename B> inline auto max(A a, B b) -> typename std::common_type<A, B>::type { return a > b ? a : b; }
#ifndef constrain
#define constrain(x, lo, hi) ((x) < (lo) ? (lo) : ((x) > (hi) ? (hi) : (x)))
#endif

class Print;
class Printable {
public:
    virtual ~Printable() {}
    virtual size_t printTo(Print& p) const = 0;
};

class Print {
public:
    virtual ~Print() {}
    virtual size_t write(uint8_t c) = 0;
    virtual size_t write(const uint8_t* buf, size_t n) { size_t k = 0; while (k < n && write(buf[k])) k++; return k; }
    size_t write(const char* s) { return s ? write((const uint8_t*)s, strlen(s)) : 0; }
    size_t write(const char* s, size_t n) { return write((const uint8_t*)s, n); }
    size_t print(const char* s) { return write(s); }
    size_t print(const String& s) { return write((const uint8_t*)s.c_str(), s.length()); }
    size_t print(char c) { return write((uint8_t)c); }
    size_t print(int v, int base = 10) { return print(String(v, (unsigned char)base)); }
    size_t print(unsigned int v, int base = 10) { return print(String(v, (unsigned char)base)); }
    size_t print(long v, int base = 10) { return print(String(v, (unsigned char)base)); }
    size_t print(unsigned long v, int base = 10) { return print(String(v, (unsigned char)base)); }
    size_t print(double v, int digits = 2) { return print(String(v, (unsigned int)digits)); }
    size_t println() { return write((const uint8_t*)"\r\n", 2); }
    template <typename T> size_t println(const T& v) { size_t n = print(v); return n + println(); }
    size_t printf(const char* fmt, ...) {
        char buf[1024];
        va_list ap; va_start(ap, fmt); int n = vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
        if (n < 0) return 0;
        return write((const uint8_t*)buf, (size_t)(n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1));
    }
    virtual void flush() {}
};

class Stream : public Print {
public:
    virtual int available() = 0;
    virtual int read() = 0;
    virtual int peek() = 0;
    void setTimeout(unsigned long) {}
    size_t readBytes(char* buf, size_t n) { size_t k = 0; int c; while (k < n && (c = read()) >= 0) buf[k++] = (char)c; return k; }
    size_t readBytes(uint8_t* buf, size_t n) { return readBytes((char*)buf, n); }
    String readString() { String s; int c; while ((c = read()) >= 0) s += (char)c; return s; }
    String readStringUntil(char t) { String s; int c; while ((c = read()) >= 0 && c != t) s += (char)c; return s; }
};

// Serial: the clock's log lines (CLOCK_LOG) go to stderr unless SIM_QUIET.
class HardwareSerial : public Print {
public:
    void begin(unsigned long) {}
    size_t write(uint8_t c) override;
    size_t write(const uint8_t* buf, size_t n) override;
    void setTxBufferSize(size_t) {}
    operator bool() const { return true; }
};
extern HardwareSerial Serial;

// Defined by each simulator: sim_modules.cpp (screens, jcsim), sim_clock_main.cpp
// (the full one, jcclock - memory as measured on the clock, restart = restart).
class EspClass {
public:
    uint32_t getFreeHeap();
    uint32_t getMaxAllocHeap();
    uint32_t getMinFreeHeap();
    uint32_t getPsramSize();
    uint32_t getFreePsram();
    [[noreturn]] void restart();
};
extern EspClass ESP;
