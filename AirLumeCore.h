#pragma once
#include <cmath>
#include <cstdint>
#include <algorithm>

namespace AirLume {
inline float clamp(float v, float lo, float hi) { return std::max(lo, std::min(hi, v)); }
inline float mix(float a, float b, float t) { return a + (b - a) * clamp(t, 0, 1); }
inline bool elapsed(uint32_t now, uint32_t start, uint32_t duration) { return uint32_t(now - start) >= duration; }

enum class ButtonAction { None, NextMode, OutputsOff, Restore };
struct TopButton {
  bool releasedOnce = false, lastRaw = false, pressed = false, handledHold = false;
  uint32_t changed = 0, down = 0;
  ButtonAction update(bool downNow, bool outputsOff, uint32_t now) {
    // A held/stuck button never blocks startup or repeatedly toggles power.
    if (!releasedOnce) {
      if (!downNow) { releasedOnce=true; changed=now; }
      return ButtonAction::None;
    }
    if (downNow != lastRaw) { lastRaw=downNow; changed=now; }
    if (!elapsed(now,changed,35)) return ButtonAction::None;
    if (downNow && !pressed) { pressed=true; handledHold=false; down=now; }
    if (pressed && downNow && !handledHold && elapsed(now,down,2000)) {
      handledHold=true;
      return outputsOff ? ButtonAction::Restore : ButtonAction::OutputsOff;
    }
    if (!downNow && pressed) {
      pressed=false;
      if (!handledHold && elapsed(now,down,40) && !elapsed(now,down,700))
        return outputsOff ? ButtonAction::Restore : ButtonAction::NextMode;
    }
    return ButtonAction::None;
  }
};

// EPA 2024 PM concentration breakpoints, interpolation, truncation and rounding.
// Passing short-term readings does NOT produce a reportable daily AQI/NowCast.
inline int pmIndex(float c, bool fine = true) {
  if (!std::isfinite(c) || c < 0) return -1;
  const float lo25[] = {0, 9.1f, 35.5f, 55.5f, 125.5f, 225.5f};
  const float hi25[] = {9, 35.4f, 55.4f, 125.4f, 225.4f, 325.4f};
  const float lo10[] = {0, 55, 155, 255, 355, 425};
  const float hi10[] = {54, 154, 254, 354, 424, 604};
  const int il[] = {0, 51, 101, 151, 201, 301};
  const int ih[] = {50, 100, 150, 200, 300, 500};
  c = fine ? std::floor(c * 10.0f + 0.0001f) / 10.0f : std::floor(c);
  const float *lo = fine ? lo25 : lo10, *hi = fine ? hi25 : hi10;
  for (int i = 0; i < 6; ++i)
    if (c <= hi[i]) return int(std::lround((ih[i] - il[i]) * (c - lo[i]) / (hi[i] - lo[i]) + il[i]));
  return 500; // Display cap, beyond scale is still hazardous.
}

struct Filter {
  float value = NAN;
  uint32_t last = 0;
  void add(float x, uint32_t now, float tauSeconds) {
    if (!std::isfinite(x)) return;
    if (!std::isfinite(value)) value = x;
    else {
      float dt = clamp(uint32_t(now - last) * 0.001f, 0, 10);
      value += (x - value) * (1.0f - std::exp(-dt / tauSeconds));
    }
    last = now;
  }
};

// Display smoothing only; valid ENS160 AQI drives alerts independently.
struct VocFilter {
  Filter filtered;
  bool candidate = false;
  uint32_t since = 0;
  void add(float raw, uint32_t now) {
    candidate=false;
    filtered.add(raw,now,10.0f);
  }
  void invalidate() { candidate = false; filtered = Filter{}; }
};

// Hysteresis and dwell time. Missing readings clear an alert's input, but the
// application separately reports SENSOR FAULT and never presents ALL OK.
struct Latch {
  bool active = false, waiting = false;
  uint32_t since = 0;
  void update(bool high, bool low, bool valid, uint32_t now, uint32_t onMs, uint32_t offMs) {
    if (!valid) { active = waiting = false; return; }
    bool change = active ? low : high;
    if (!change) { waiting = false; return; }
    if (!waiting) { waiting = true; since = now; }
    if (elapsed(now, since, active ? offMs : onMs)) { active = !active; waiting = false; }
  }
};

inline float fahrenheit(float c) { return c*1.8f+32; }
// WHO PM reference levels are 24-hour means. These LIVE advisory triggers
// use those concentrations with a 60-second dwell, NOT a WHO compliance test.
constexpr float PM25_ADVISORY = 15, PM10_ADVISORY = 45;
constexpr uint16_t CO2_VENTILATE = 1500, CO2_ATTENTION = 2500, CO2_HIGH = 5000;
// High live-reading precautions inspired by EPA's 24-hour unhealthy PM
// breakpoints. Not an instantaneous medical danger assessment or calculated AQI.
constexpr float PM25_HIGH = 55.5f, PM10_HIGH = 255;
struct AirSignal {bool elevated=false, high=false;float yellow=0;};
inline AirSignal airSignal(bool cf,float co2,bool pf,float fine,float coarse,bool vf,uint8_t aqi) {
  AirSignal out;
  out.high=(cf && co2>=CO2_HIGH) || (pf && (fine>=PM25_HIGH || coarse>=PM10_HIGH)) || (vf && aqi>=5);
  out.elevated=(cf && co2>=CO2_VENTILATE) || (pf && (fine>PM25_ADVISORY || coarse>PM10_ADVISORY)) || (vf && aqi>=3);
  if(cf)out.yellow=std::max(out.yellow,clamp((co2-CO2_VENTILATE)/(CO2_HIGH-CO2_VENTILATE),0,1));
  if(pf)out.yellow=std::max(out.yellow,std::max(clamp((fine-PM25_ADVISORY)/(PM25_HIGH-PM25_ADVISORY),0,1),clamp((coarse-PM10_ADVISORY)/(PM10_HIGH-PM10_ADVISORY),0,1)));
  if(vf && aqi>=3)out.yellow=std::max(out.yellow,aqi>=4?0.75f:0.25f);
  return out;
}
inline bool particlesElevated(float fine, float coarse) {
  return fine>PM25_ADVISORY || coarse>PM10_ADVISORY;
}
// Product severity inputs, not a regulatory or WHO AQI.
// CO2 is a ventilation indicator; 1200 ppm does not trigger a warning/color.
inline float co2Score(float ppm) {
  if (ppm <= CO2_VENTILATE) return 0;
  return mix(0,200,(ppm-CO2_VENTILATE)/(CO2_ATTENTION-CO2_VENTILATE));
}
inline float vocScore(float ppb) {
  if (ppb <= 200) return 0;
  if (ppb <= 650) return mix(0, 100, (ppb - 200) / 450);
  return mix(100, 200, (ppb - 650) / 1550);
}
constexpr float HUMIDITY_LOW = 40, HUMIDITY_HIGH = 60;
inline const char *humidityAdvice(float rh) {
  if(rh<HUMIDITY_LOW)return "ADD HUMIDITY";
  if(rh>HUMIDITY_HIGH)return "REDUCE HUMIDITY";
  return "COMFORT RECOVERING";
}
inline float comfortScore(float t, float rh) {
  float td = std::max(18 - t, t - 26), hd = std::max(40 - rh, rh - 60);
  return std::max(clamp(td / 6, 0, 1), clamp(hd / 20, 0, 1)) * 90;
}
struct RGB { float r = 0, g = 0, b = 0; };
inline RGB blend(RGB a, RGB b, float t) { return {mix(a.r,b.r,t), mix(a.g,b.g,t), mix(a.b,b.b,t)}; }
inline RGB severityColor(float v) {
  // Deliberate product gradient, not the EPA's categorical color palette.
  v = clamp(v, 0, 200);
  if (v <= 100) return blend({0,255,0}, {255,200,0}, v / 100);
  return blend({255,200,0}, {255,0,0}, (v - 100) / 100);
}
}
