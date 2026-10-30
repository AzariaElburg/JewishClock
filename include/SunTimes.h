#pragma once

// Sunset and tzeit computed on the clock itself, from the place's latitude/
// longitude - NOAA's solar position formulas (the same ones Hebcal uses).
// Used only when the downloaded calendar has no time for today: Hebcal's own
// times always win. Checked against a year of Hebcal's times for several
// places (2026-10-27): see SunTimes.cpp.
namespace SunTimes {

// How far below the horizon the sun's centre is at each moment, in degrees.
constexpr double SUNSET_DEG = 0.833;   // sunset: refraction + half the sun's disc
constexpr double TZEIT_DEG  = 7.083;   // צאת הכוכבים - Hebcal's tzeit7083deg

// Minutes after 00:00 UTC of Gregorian date y-m-d at which the setting sun is
// `belowDeg` below the horizon (may be < 0 or >= 1440 far from Greenwich).
// Returns false when the sun doesn't get that low that evening (far north
// in summer).
bool eveningUtcMin(int y, int m, int d, double lat, double lon, double belowDeg, double& utcMin);

}
