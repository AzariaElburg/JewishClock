#pragma once
#include <cstdint>
// PC stand-in (simulator only): there is no SPI bus on the PC.
#define FSPI 0
#define HSPI 1
struct spi_struct_t;
typedef struct spi_struct_t spi_t;
inline uint32_t spiFrequencyToClockDiv(spi_t*, uint32_t) { return 0; }   // core 3.3.6+ signature
