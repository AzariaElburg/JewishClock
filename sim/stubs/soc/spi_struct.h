#pragma once
#include <cstdint>
// PC stand-in (simulator only): the two registers Display.cpp touches.
typedef struct {
    struct { volatile uint32_t usr; } cmd;
    struct { volatile uint32_t val; } clock;
} spi_dev_t;
extern spi_dev_t GPSPI2;
extern spi_dev_t GPSPI3;
