#include "SunTimes.h"
#include "JewishDateEngine.h"
#include <math.h>

// NOAA's solar calculator (Meeus, "Astronomical Algorithms"): the sun's
// declination and the equation of time at a moment, then the hour angle at
// which it stands `belowDeg` under the horizon. Recomputed at the result a few
// times, since the sun's position moves during the day.

static const double PI_ = 3.14159265358979323846;   // M_PI isn't standard C++ (the simulator lacks it)
static double rad(double d) { return d * PI_ / 180.0; }
static double deg(double r) { return r * 180.0 / PI_; }

bool SunTimes::eveningUtcMin(int y, int m, int d, double lat, double lon, double belowDeg, double& utcMin) {
    // Julian day at 00:00 UTC (absolute day 0 = 1721424.5).
    double jd0 = (double)JewishDateEngine::gregorianToAbsolute(y, m, d) + 1721424.5;
    double t = 720.0 - 4.0 * lon;   // first guess: solar noon
    for (int i = 0; i < 3; i++) {
        double T  = (jd0 + t / 1440.0 - 2451545.0) / 36525.0;   // Julian centuries since J2000
        double L0 = fmod(280.46646 + T * (36000.76983 + T * 0.0003032), 360.0);
        double M  = 357.52911 + T * (35999.05029 - 0.0001537 * T);
        double e  = 0.016708634 - T * (0.000042037 + 0.0000001267 * T);
        double C  = sin(rad(M)) * (1.914602 - T * (0.004817 + 0.000014 * T))
                  + sin(rad(2 * M)) * (0.019993 - 0.000101 * T) + sin(rad(3 * M)) * 0.000289;
        double omega  = 125.04 - 1934.136 * T;
        double lambda = L0 + C - 0.00569 - 0.00478 * sin(rad(omega));
        double eps    = 23.0 + (26.0 + (21.448 - T * (46.815 + T * (0.00059 - T * 0.001813))) / 60.0) / 60.0
                      + 0.00256 * cos(rad(omega));
        double decl = asin(sin(rad(eps)) * sin(rad(lambda)));
        double yv   = tan(rad(eps / 2)) * tan(rad(eps / 2));
        double eqTime = 4.0 * deg(yv * sin(2 * rad(L0)) - 2 * e * sin(rad(M))
                                  + 4 * e * yv * sin(rad(M)) * cos(2 * rad(L0))
                                  - 0.5 * yv * yv * sin(4 * rad(L0)) - 1.25 * e * e * sin(2 * rad(M)));
        double cosH = (cos(rad(90.0 + belowDeg)) - sin(rad(lat)) * sin(decl)) / (cos(rad(lat)) * cos(decl));
        if (cosH < -1.0 || cosH > 1.0) return false;
        t = 720.0 - 4.0 * lon - eqTime + 4.0 * deg(acos(cosH));
    }
    utcMin = t;
    return true;
}
