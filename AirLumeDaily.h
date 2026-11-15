#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace AirLume {
// Piecewise-constant time-weighted means of raw readings. An expired reading
// contributes neither concentration nor time; gaps are never counted as zero.
struct DailyMetric {
  double integral = 0;
  uint32_t coveredMs = 0, stamp = 0, ttl = 0;
  float held = NAN;
  void observe(float value, uint32_t measuredAt, uint32_t freshFor) {
    held = std::isfinite(value) && value >= 0 ? value : NAN;
    stamp = measuredAt; ttl = freshFor;
  }
  void integrate(uint32_t from, uint32_t to) {
    const uint32_t age = uint32_t(from - stamp);
    if (!std::isfinite(held) || age >= ttl) return;
    const uint32_t duration = std::min(uint32_t(to - from), ttl - age);
    integral += double(held) * duration;
    coveredMs += duration;
  }
  float mean() const { return coveredMs ? float(integral / coveredMs) : NAN; }
  void resetTotals() { integral = 0; coveredMs = 0; }
};
struct DailyMonitor {
  static constexpr uint32_t DAY_MS = 86400000;
  DailyMetric co2, pm25, pm10, voc;
  uint32_t start = 0, cursor = 0, period = 1;
  void begin(uint32_t now) { *this = DailyMonitor{}; start = cursor = now; }
  void integrate(uint32_t end) {
    for (DailyMetric *m : {&co2, &pm25, &pm10, &voc}) m->integrate(cursor, end);
    cursor = end;
  }
  void advance(uint32_t now) {
    // Unsigned differences remain correct through millis() rollover.
    while (uint32_t(now - start) >= DAY_MS) {
      integrate(start + DAY_MS);
      for (DailyMetric *m : {&co2, &pm25, &pm10, &voc}) m->resetTotals();
      start += DAY_MS; ++period;
    }
    integrate(now);
  }
};
}
