/* AirLume | ESP32-C6 Super Mini | 200 RGB WS2812/NeoPixels
   Open AirLumeSerial.ino. See ../README.md for wiring and alert policy.
   Live yellow/red thresholds are product settings, not an instantaneous
   health assessment or regulatory AQI. No overall perceived score is used.
   Prototype environmental indicator, not a smoke/CO/life-safety alarm.
*/
#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_NeoPixel.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <Adafruit_AHTX0.h>
#include <SensirionI2cScd4x.h>
#include <SensirionI2cSps30.h>
#include <esp_wifi.h>
#include "AirLumeCore.h"
using namespace AirLume;

// ---- Hardware settings: GPIO numbers, not physical header positions ----
constexpr int SDA_PIN = 6, SCL_PIN = 7;
constexpr int LED_PIN = 2;       // WS2812B strip DIN
constexpr int BUTTON_PIN = 3;     // Button to GND; internal pullup, optional external 10k
constexpr int LED_COUNT = 200;
constexpr uint8_t OLED_ADDR = 0x3C, OLED_ROTATION = 0;
constexpr bool FIRE_REVERSED = false; // False: pixel 0 is physical bottom of helix
constexpr bool PAUSE_SENSORS_WHEN_OFF = false; // Monitoring continues even with lamp/display switched off
constexpr uint32_t CPU_MHZ = 160; // Restore the normal C6 clock while resolving the panic
constexpr float LED_BUDGET_MA = 1000; // Estimated strip current limit with separate 5 V supply
constexpr uint8_t NIGHT_BRIGHTNESS = 8;
constexpr uint8_t LED_BRIGHTNESS = 150; // Target before current-budget scaling
constexpr RGB WARM_GOLD = {255,165,65}; // Approximate 2700 K look on this RGB strip
constexpr RGB NEUTRAL_WHITE = {255,200,140}; // The previous Warm White looked white on this strip
constexpr bool TRACE_FIRST_LOOP = false;
constexpr uint32_t PAGE_MS = 5000, INTRO_MS = 3000;
// Provisional room calibration: prior corrected 81.8 F versus reference 73 F.
// Previous -3 C plus an additional -8.8 F / 1.8 = total -7.8889 C.
// Averaging alone cannot remove a systematic heat bias in both sensors.
constexpr float SCD_T_CORRECTION = -7.888889f, AHT_T_CORRECTION = -7.888889f;
constexpr float SCD_RH_CORRECTION = 0, AHT_RH_CORRECTION = 0;
static_assert(LED_COUNT >= 8, "Fire simulation needs at least eight pixels");

Adafruit_NeoPixel pixels(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);
Adafruit_SH1107 oled(128, 128, &Wire, -1, 100000, 100000);
Adafruit_AHTX0 aht;
SensirionI2cScd4x scd;
SensirionI2cSps30 sps;

enum Mode { MONITOR, WARM_WHITE, COOL_WHITE, FIRE, NIGHT };
constexpr int MODE_COUNT = 5;
Mode mode = MONITOR; // Power on as the visual air-quality monitor
const char *modeName[] = {"GREEN / RED", "WARM WHITE", "WHITE", "FIREPLACE", "NIGHT"};
bool screenOK = false, ahtOnline = false, scdOnline = false, spsOnline = false, ensOnline = false;
bool warming = true, outputsOff = false, sensorsPaused = false;
TopButton topButton;
uint8_t ensAddress = 0, ensValidity = 3, ensUBA = 0;
uint16_t tvocRaw = 0, eco2 = 0, co2Raw = 0;
uint32_t ensSeen = 0, vocSeen = 0, scdSeen = 0, ahtSeen = 0, spsSeen = 0;
uint32_t startMs = 0, ensStartMs = 0, recoveryMs = 0, modeNoticeMs = 0;
uint32_t pageEpoch = 0, modelTick = 0;
float scdT = NAN, scdRH = NAN, ahtT = NAN, ahtRH = NAN;
float tempAvg = NAN, rhAvg = NAN;
float pm[4] = {NAN,NAN,NAN,NAN}, counts[5] = {NAN,NAN,NAN,NAN,NAN}, particleSize = NAN;
AirSignal air;
Filter co2Smooth, pm25Smooth, pm10Smooth, tempSmooth, rhSmooth;
VocFilter voc;
Latch co2Warning, co2Attention, co2Urgent, pmWarning, vocWarning, comfortWarning;
bool fault = false, issue = false, co2Fresh = false, pmFresh = false, vocFresh = false, thFresh = false;
RGB rendered[LED_COUNT];
float heat[LED_COUNT] = {};
alignas(4) uint8_t oledFrame[2048];
int oledPage = -1;

// ---- Minimal ENS160 register driver, following ScioSense datasheet v1.3 ----
bool readReg(uint8_t addr, uint8_t reg, uint8_t *data, size_t n) {
  Wire.beginTransmission(addr); Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(addr, uint8_t(n)) != n) { while (Wire.available()) Wire.read(); return false; }
  for (size_t i=0; i<n; ++i) data[i] = Wire.read();
  return true;
}
bool writeReg(uint8_t addr, uint8_t reg, const uint8_t *data, size_t n) {
  Wire.beginTransmission(addr); Wire.write(reg); Wire.write(data,n);
  return Wire.endTransmission() == 0;
}
bool ensMode(uint8_t m) { return ensAddress && writeReg(ensAddress,0x10,&m,1); }
bool initENS() {
  uint8_t id[2];
  for (uint8_t addr : {uint8_t(0x53),uint8_t(0x52)}) {
    if (!readReg(addr,0x00,id,2) || (uint16_t(id[1])<<8 | id[0]) != 0x0160) continue;
    ensAddress=addr;
    if (!ensMode(0x01)) continue; // IDLE first, do not reset stored baselines
    delay(10);
    if (ensMode(0x02)) { ensStartMs=millis(); ensValidity=3; return true; }
  }
  return false;
}
void pollENS(uint32_t now) {
  if (!ensOnline) return;
  uint8_t status;
  if (!readReg(ensAddress,0x20,&status,1)) return;
  ensSeen=now; ensValidity=(status>>2)&3;
  if ((status&0x40) || !(status&0x80)) { ensValidity=3; return; }
  if (!(status&0x02)) return;
  uint8_t data[5];
  if (!readReg(ensAddress,0x21,data,5)) return;
  // Read the whole output block even during warmup to consume NEWDAT correctly.
  if (ensValidity != 0) return;
  ensUBA=data[0]&7;
  if (ensUBA < 1 || ensUBA > 5) return;
  tvocRaw=uint16_t(data[2])<<8 | data[1];
  eco2=uint16_t(data[4])<<8 | data[3];
  vocSeen=now; voc.add(tvocRaw,now);
}
void compensateENS() {
  if (!ensOnline || !std::isfinite(tempAvg) || !std::isfinite(rhAvg)) return;
  uint16_t t=uint16_t(std::lround((clamp(tempAvg,-40,85)+273.15f)*64));
  uint16_t h=uint16_t(std::lround(clamp(rhAvg,0,100)*512));
  uint8_t data[]={uint8_t(t),uint8_t(t>>8),uint8_t(h),uint8_t(h>>8)};
  writeReg(ensAddress,0x13,data,4);
}

// ---- Sensor service; only new successfully read measurements get timestamps ----
bool fresh(uint32_t stamp, uint32_t now, uint32_t limit) { return stamp != 0 && !elapsed(now,stamp,limit); }
void initSCD() {
  scd.begin(Wire,0x62);
  scd.stopPeriodicMeasurement(); delay(500);
  // Do not write user calibration, factory offsets or ASC settings here.
  scdOnline = scd.startPeriodicMeasurement() == 0;
}
void initSPS() {
  sps.begin(Wire,0x69);
  sps.wakeUpSequence(); delay(20);
  sps.stopMeasurement(); delay(20);
  spsOnline = sps.startMeasurement(SPS30_OUTPUT_FORMAT_OUTPUT_FORMAT_FLOAT) == 0;
}
void pollSensors(uint32_t now) {
  static uint32_t scdPoll=0, ahtPoll=0, spsPoll=0, ensPoll=0, retry=0, comp=0;
  if (elapsed(now,scdPoll,1000)) {
    scdPoll=now; bool ready=false;
    if (scdOnline && scd.getDataReadyStatus(ready)==0 && ready) {
      uint16_t c; float t,h;
      if (scd.readMeasurement(c,t,h)==0 && c>0 && std::isfinite(t) && std::isfinite(h) && h>=0 && h<=100) {
        co2Raw=c; scdT=t+SCD_T_CORRECTION; scdRH=clamp(h+SCD_RH_CORRECTION,0,100);
        scdSeen=now; co2Smooth.add(c,now,10);
      }
    }
  }
  if (elapsed(now,ahtPoll,2000)) {
    ahtPoll=now;
    if (ahtOnline) {
      sensors_event_t humidity,temperature;
      if (aht.getEvent(&humidity,&temperature) && std::isfinite(temperature.temperature) &&
          std::isfinite(humidity.relative_humidity) && humidity.relative_humidity>=0 && humidity.relative_humidity<=100) {
        ahtT=temperature.temperature+AHT_T_CORRECTION;
        ahtRH=clamp(humidity.relative_humidity+AHT_RH_CORRECTION,0,100); ahtSeen=now;
      }
    }
  }
  if (elapsed(now,spsPoll,1000)) {
    spsPoll=now; uint16_t ready=0;
    if (spsOnline && sps.readDataReadyFlag(ready)==0 && ready) {
      float v[10];
      if (sps.readMeasurementValuesFloat(v[0],v[1],v[2],v[3],v[4],v[5],v[6],v[7],v[8],v[9])==0) {
        bool valid=true; for (float x:v) valid &= std::isfinite(x) && x>=0;
        if (valid) {
          for(int i=0;i<4;++i) pm[i]=v[i];
          for(int i=0;i<5;++i) counts[i]=v[i+4];
          particleSize=v[9]; spsSeen=now;
          pm25Smooth.add(pm[1],now,15); pm10Smooth.add(pm[3],now,15);
        }
      }
    }
  }
  if (elapsed(now,ensPoll,1000)) { ensPoll=now; pollENS(now); }
  if (elapsed(now,comp,5000)) { comp=now; compensateENS(); }
  if (elapsed(now,retry,30000)) {
    retry=now;
    if (!fresh(scdSeen,now,30000)) initSCD();
    if (!fresh(spsSeen,now,30000)) initSPS();
    if (!fresh(ahtSeen,now,30000)) ahtOnline=aht.begin(&Wire);
    if (!fresh(ensSeen,now,30000)) ensOnline=initENS();
    if (!screenOK && !outputsOff) { screenOK=oled.begin(OLED_ADDR,true) && oled.getBuffer()!=nullptr; if(screenOK) {oled.setRotation(OLED_ROTATION);oled.cp437(true);} }
  }
}

// ---- Scoring and alert policy ----
bool ensConditioning(uint32_t now) {
  return ensOnline && fresh(ensSeen,now,10000) && (ensValidity==1 || ensValidity==2);
}
void updateModel(uint32_t now) {
  bool sf=fresh(scdSeen,now,20000), af=fresh(ahtSeen,now,15000);
  thFresh=sf || af; co2Fresh=sf; pmFresh=fresh(spsSeen,now,10000);
  bool previousVocFresh=vocFresh;
  vocFresh=ensOnline && ensValidity==0 && fresh(vocSeen,now,10000);
  if(previousVocFresh!=vocFresh)pageEpoch=now;
  if(!co2Fresh)co2Smooth=Filter{};
  if(!pmFresh){pm25Smooth=Filter{};pm10Smooth=Filter{};}
  if(!thFresh){tempSmooth=Filter{};rhSmooth=Filter{};}
  if (sf && af) {tempAvg=(scdT+ahtT)/2; rhAvg=(scdRH+ahtRH)/2;}
  else if (sf) {tempAvg=scdT;rhAvg=scdRH;}
  else if (af) {tempAvg=ahtT;rhAvg=ahtRH;}
  else {tempAvg=rhAvg=NAN;}
  if(thFresh) {tempSmooth.add(tempAvg,now,20);rhSmooth.add(rhAvg,now,20);}
  if (!vocFresh && !ensConditioning(now)) voc.invalidate();
  // Missing channels are never treated as evidence that the air is good.
  bool pastGrace=elapsed(now,startMs,20000);
  bool warmupTimeout=elapsed(now,ensStartMs,3900000) && !vocFresh;
  fault=pastGrace && (!sf || !pmFresh || (!vocFresh && !ensConditioning(now)) || warmupTimeout || !screenOK);
  air=airSignal(co2Fresh,float(co2Raw),pmFresh,pm[1],pm[3],vocFresh,ensUBA);

  // CO2 alert decisions use fresh actual SCD40 CO2, never ENS160 eCO2.
  // Only reevaluate CO2 dwell on a NEW SCD reading; repeated polls cannot
  // manufacture a second confirmation from one stale sample.
  static uint32_t usedSCD=0;
  if (co2Fresh && usedSCD!=scdSeen) {
    usedSCD=scdSeen;
    co2Warning.update(co2Raw>=CO2_VENTILATE,co2Raw<1300,true,now,30000,60000);
    co2Attention.update(co2Raw>=CO2_ATTENTION,co2Raw<2000,true,now,10000,60000);
    co2Urgent.update(co2Raw>=CO2_HIGH,co2Raw<4000,true,now,0,60000);
  } else if (!co2Fresh) {
    co2Warning.update(false,false,false,now,0,0);
    co2Attention.update(false,false,false,now,0,0);
    co2Urgent.update(false,false,false,now,0,0);
  }
  pmWarning.update(particlesElevated(pm25Smooth.value,pm10Smooth.value),
    pm25Smooth.value<12 && pm10Smooth.value<36,pmFresh,now,60000,60000);
  // ENS160 manufacturer AQI 4/5 = poor/unhealthy; not a WHO TVOC limit.
  vocWarning.update(vocFresh && ensUBA>=4,vocFresh && ensUBA<=2,vocFresh,now,30000,60000);
  comfortWarning=Latch{}; // No temperature/humidity decisions or alerts.
  bool oldIssue=issue;
  issue=co2Warning.active || co2Attention.active || co2Urgent.active || pmWarning.active || vocWarning.active || fault;
  if (oldIssue && !issue && !ensConditioning(now) && vocFresh && pmFresh && co2Fresh && sf) recoveryMs=now;
  if (issue) recoveryMs=0;

}

// ---- Radio stays disabled; no access point, network or web server ----
void turnOffWiFi() {
  esp_err_t e=esp_wifi_stop();
  if(e!=ESP_OK && e!=ESP_ERR_WIFI_NOT_INIT)Serial.printf("WiFi stop: %d\n",int(e));
  e=esp_wifi_deinit();
  if(e!=ESP_OK && e!=ESP_ERR_WIFI_NOT_INIT)Serial.printf("WiFi deinit: %d\n",int(e));
}

// ---- Light engine: spatial fire simulation, transitions and current estimate ----
RGB fireColor(float h) {
  h=clamp(h,0,1);
  if(h<0.35f) return {h/0.35f*255,0,0};
  if(h<0.75f) return {255,(h-0.35f)/0.4f*160,0};
  return {255,160+(h-0.75f)/0.25f*80,(h-0.75f)/0.25f*75};
}
void stepFire() {
  // Original implementation: cool, advect heat upward, ignite at the base.
  // One continuous strand; no 2D matrix mapping required.
  for(int i=LED_COUNT-1;i>0;--i) {
    float below=heat[i-1], below2=heat[std::max(0,i-2)];
    heat[i]=std::max(0.0f,heat[i]*0.4f+below*0.4f+below2*0.2f-random(2,18)/1000.0f);
  }
  heat[0]=std::max(0.0f,heat[0]-0.055f);
  if(random(100)<55) {int j=random(6);heat[j]=clamp(heat[j]+random(30,80)/100.0f,0,1);}
}
void renderLEDs(uint32_t now) {
  if(outputsOff || pixels.getPixels()==nullptr)return;
  static uint32_t frame=0;
  if(!elapsed(now,frame,33))return;
  float dt=clamp(uint32_t(now-frame)*0.001f,0.001f,0.1f);frame=now;
  stepFire();
  bool night=mode==NIGHT;
  float brightness=(night?NIGHT_BRIGHTNESS:LED_BRIGHTNESS)/255.0f;
  bool elevated=air.elevated || co2Warning.active || co2Attention.active || pmWarning.active || vocWarning.active;
  bool red=air.high || co2Urgent.active;
  bool urgent=red;
  float pulse=(1-std::cos(2*PI*(now%1200)/1200.0f))*0.5f;
  RGB indicator={0,255,0}; // Green/red mode responds to confirmed pollutant alerts only
  float sumChannels=0;
  bool modeChanging=modeNoticeMs && !elapsed(now,modeNoticeMs,1500);
  float transition=1-std::exp(-dt/(elevated || red || modeChanging?0.12f:1.1f));
  for(int i=0;i<LED_COUNT;++i) {
    RGB target=indicator;
    if(mode==WARM_WHITE || mode==NIGHT)target=WARM_GOLD;
    if(mode==COOL_WHITE)target=NEUTRAL_WHITE;
    if(mode==FIRE)target=fireColor(heat[FIRE_REVERSED?LED_COUNT-1-i:i]);
    // Decorative modes keep their selected color. Monitoring runs independently.
    if(mode==MONITOR && (!co2Fresh || !pmFresh))target={35,90,255};
    if(fault)target=blend(target,{255,110,0},0.55f);
    // Elevated pollution is yellow; high live readings override every mode red.
    if(elevated)target=blend({255,235,120},{255,185,0},air.yellow);
    if(red)target={255,0,0};
    if(urgent)brightness=((night?NIGHT_BRIGHTNESS:LED_BRIGHTNESS)/255.0f)*(0.15f+0.85f*pulse);
    target.r*=brightness;target.g*=brightness;target.b*=brightness;
    rendered[i]=blend(rendered[i],target,transition);
    sumChannels+=rendered[i].r+rendered[i].g+rendered[i].b;
  }
  // Estimate 1 mA idle per pixel plus 20 mA per full-scale RGB channel.
  // This is a software estimate, not a measurement of supply current.
  float estimated=LED_COUNT+sumChannels*(20.0f/255);
  float powerScale=estimated>LED_BUDGET_MA?
      clamp((LED_BUDGET_MA-LED_COUNT)/(estimated-LED_COUNT),0,1):1;
  for(int i=0;i<LED_COUNT;++i) pixels.setPixelColor(i,
      uint8_t(rendered[i].r*powerScale),uint8_t(rendered[i].g*powerScale),uint8_t(rendered[i].b*powerScale));
  pixels.show();
}

// ---- OLED typography and original lightweight procedural animations ----
void centered(const String &s,int y,int size) {
  oled.setTextSize(size);int16_t x1,y1;uint16_t w,h;
  oled.getTextBounds(s,0,y,&x1,&y1,&w,&h);
  oled.setCursor((128-int(w))/2-x1,y);oled.print(s);
}
void metric(const char *label,float value,const char *unit,int decimals=0,const char *foot="") {
  int headingSize=strncmp(label,"PM ",3)==0?3:1;
  centered(label,3,headingSize);
  String text=std::isfinite(value)?String(value,decimals):String("--");
  int size=5;while(size>1 && int(text.length())*6*size>122)--size;
  centered(text,32,size);centered(unit,77,2);centered(foot,101,1);
}
void windowAnimation(uint32_t now,int top=74) {
  int opening=int((1-std::cos(2*PI*(now%2600)/2600.0f))*10);
  oled.drawRect(42,top,44,30,SH110X_WHITE);oled.drawLine(64,top,64,top+30,SH110X_WHITE);
  oled.drawLine(43,top+1,60-opening,top+5,SH110X_WHITE);
  oled.drawLine(60-opening,top+5,60-opening,top+25,SH110X_WHITE);
  oled.drawLine(60-opening,top+25,43,top+29,SH110X_WHITE);
  int x=12+int((now/50)%18);
  for(int j=0;j<2;++j){int y=top+9+j*12;oled.drawFastHLine(x,y,18,SH110X_WHITE);oled.drawLine(x+18,y,x+13,y-3,SH110X_WHITE);oled.drawLine(x+18,y,x+13,y+3,SH110X_WHITE);}
}
void checkAnimation(uint32_t now) {
  oled.drawCircle(64,64,29,SH110X_WHITE);
  float p=clamp((now%2200)/900.0f,0,1);
  int x=p<0.35f?int(mix(47,59,p/0.35f)):int(mix(59,82,(p-0.35f)/0.65f));
  int y=p<0.35f?int(mix(64,76,p/0.35f)):int(mix(76,49,(p-0.35f)/0.65f));
  if(p<0.35f) {oled.drawLine(47,64,x,y,SH110X_WHITE);oled.drawLine(47,65,x,y+1,SH110X_WHITE);}
  else {oled.drawLine(47,64,59,76,SH110X_WHITE);oled.drawLine(47,65,59,77,SH110X_WHITE);oled.drawLine(59,76,x,y,SH110X_WHITE);oled.drawLine(59,77,x,y+1,SH110X_WHITE);}
}
void filterAnimation(uint32_t now) {
  oled.drawRect(57,75,15,30,SH110X_WHITE);
  for(int x=60;x<72;x+=4)oled.drawFastVLine(x,78,24,SH110X_WHITE);
  for(int i=0;i<4;++i)oled.fillCircle(14+((now/70+i*11)%38),79+i*7,1,SH110X_WHITE);
  oled.drawFastHLine(78+(now/80)%12,90,16,SH110X_WHITE);
}
void warningPage(const char *title,float value,const char *unit,const char *action,int icon,uint32_t now) {
  centered(title,3,strncmp(title,"PM ",3)==0?3:2);
  String n=std::isfinite(value)?String(value,0):"--";
  int size=n.length()>4?3:4;centered(n,30,size);centered(unit,63,1);
  if(icon==1)windowAnimation(now);else if(icon==2)filterAnimation(now);
  centered(action,114,1);
}
void drawFault(uint32_t now) {
  centered("CHECK",5,2);centered("SENSORS",27,2);
  int y=53;
  if(!co2Fresh){centered("SCD40: NO CO2",y,1);y+=12;}
  if(!pmFresh){centered("SPS30: NO PM",y,1);y+=12;}
  if(!fresh(ahtSeen,now,15000)){centered("AHT: NO T/RH",y,1);y+=12;}
  if(!vocFresh && (!ensConditioning(now) || elapsed(now,ensStartMs,3900000))){centered("ENS160: NOT READY",y,1);y+=12;}
  if(!screenOK){centered("OLED FAILED",y,1);y+=12;}
  centered("CHECK WIRING/POWER",115,1);
}
void comparisonBar(float value,float reference,float maximum) {
  constexpr int x=8,y=115,w=112,h=8;
  oled.drawRect(x,y,w,h,SH110X_WHITE);
  if(std::isfinite(value))oled.fillRect(x+1,y+2,int((w-2)*clamp(value/maximum,0,1)),h-4,SH110X_WHITE);
  int marker=x+1+int((w-3)*clamp(reference/maximum,0,1));
  oled.drawFastVLine(marker,y-2,h+4,SH110X_WHITE);
}
void pmWarningPage(bool fine,float value,bool high,bool falling) {
  centered(fine?"PM 2.5":"PM 10",3,3);
  centered(high?"HIGH READING":(falling?"FALLING":"ELEVATED"),28,1);
  String n=std::isfinite(value)?String(value,1):"--";
  int size=4;while(size>1 && int(n.length())*6*size>122)--size;
  centered(n,38,size);
  centered("ug/m3",74,2);
  centered(high?"CLEAR THE AIR":"FILTER / VENTILATE",92,1);
  float redAt=fine?PM25_HIGH:PM10_HIGH;
  centered(fine?"YELLOW>15 RED>=55.5":"YELLOW>45 RED>=255",103,1);
  comparisonBar(value,redAt,redAt*1.5f);
}
void humidityBar(float value) {
  constexpr int x=8,y=115,w=112,h=8;
  oled.drawRect(x,y,w,h,SH110X_WHITE);
  for(int pct:{40,60})oled.drawFastVLine(x+1+int((w-3)*pct/100.0f),y-2,h+4,SH110X_WHITE);
  if(std::isfinite(value))oled.fillRect(x+1+int((w-5)*clamp(value/100,0,1)),y+2,3,h-4,SH110X_WHITE);
}
void drawWarmup(uint32_t now) {
  // Large, bold centered wordmark, shown from initial display setup.
  const char *name="AirLume";
  oled.setTextSize(3);int16_t x1,y1;uint16_t w,h;
  oled.getTextBounds(name,0,46,&x1,&y1,&w,&h);
  int x=(128-int(w)-1)/2-x1;
  oled.setCursor(x,46);oled.print(name);
  oled.setCursor(x+1,46);oled.print(name);
  centered("Initializing...",80,1);
  int head=(now/100)%8;
  for(int i=0;i<8;++i){float angle=i*PI/4;
    oled.fillCircle(64+int(9*std::cos(angle)),107+int(9*std::sin(angle)),i==head?2:1,SH110X_WHITE);}
}
void drawCycle(uint32_t now) {
  const int pagesWithVoc[]={0,1,2,3,4,5,7};
  const int pagesWithoutVoc[]={0,1,2,3,4,7};
  int count=vocFresh?7:6;
  int slot=((now-pageEpoch)/PAGE_MS)%count;
  int page=vocFresh?pagesWithVoc[slot]:pagesWithoutVoc[slot];
  switch(page) {
    case 0:metric("CARBON DIOXIDE",co2Fresh?co2Smooth.value:NAN,"ppm",0,"Alert ref: 1500 ppm");
      comparisonBar(co2Fresh?co2Smooth.value:NAN,1500,3000);break;
    case 1:metric("TEMPERATURE",thFresh?fahrenheit(tempSmooth.value):NAN,"\xF8" "F",1,"Under 80 F: OK");
      comparisonBar(thFresh?fahrenheit(tempSmooth.value):NAN,80,100);break;
    case 2:metric("HUMIDITY",thFresh?rhSmooth.value:NAN,"% RH",0,"Comfort ref: 40-60%");
      humidityBar(thFresh?rhSmooth.value:NAN);break;
    case 3:metric("PM 2.5",pmFresh?pm25Smooth.value:NAN,"ug/m3",1,"WHO 24h ref: 15");
      comparisonBar(pmFresh?pm25Smooth.value:NAN,15,45);break;
    case 4:metric("PM 10",pmFresh?pm10Smooth.value:NAN,"ug/m3",1,"WHO 24h ref: 45");
      comparisonBar(pmFresh?pm10Smooth.value:NAN,45,135);break;
    case 5:{char foot[24];snprintf(foot,sizeof(foot),"Sensor AQI: %u/5",ensUBA);
      metric("VOC ESTIMATE",voc.filtered.value,"ppb",0,foot);
      comparisonBar(float(ensUBA),4,5);break;}
    case 7:metric("LAMP MODE",float(mode+1),"/ 5",0,modeName[mode]);
      comparisonBar(float(mode+1),5,5);break;
  }
  // Thin bottom line shows the five-second page dwell separately from value bars.
  oled.drawFastHLine(0,127,int(128*((now-pageEpoch)%PAGE_MS)/PAGE_MS),SH110X_WHITE);
}
void serviceDisplayBrightness() {
  static int previous=-1;
  if(!screenOK || outputsOff){previous=-1;return;}
  int contrast=mode==NIGHT?NIGHT_BRIGHTNESS:LED_BRIGHTNESS;
  if(contrast!=previous){oled.setContrast(contrast);previous=contrast;}
}
void serviceIntro(uint32_t now) {
  if(warming && elapsed(now,startMs,INTRO_MS)){warming=false;pageEpoch=now;}
}
void renderOLED(uint32_t now) {
  static uint32_t frame=0;
  if(outputsOff || !screenOK || oledPage>=0 || !elapsed(now,frame,125))return;
  if(oled.getBuffer()==nullptr){screenOK=false;oledPage=-1;return;}
  frame=now;oled.clearDisplay();oled.setTextColor(SH110X_WHITE);oled.setTextWrap(false);
  serviceIntro(now);
  if(air.high) {
    if(co2Fresh && co2Raw>=CO2_HIGH)warningPage("HIGH CO2",co2Raw,"ppm","GET FRESH AIR",1,now);
    else if(pmFresh && pm[1]>=PM25_HIGH)pmWarningPage(true,pm[1],true,false);
    else if(pmFresh && pm[3]>=PM10_HIGH)pmWarningPage(false,pm[3],true,false);
    else warningPage("VOC HIGH",float(ensUBA),"/5 SENSOR AQI","REMOVE SOURCE / AIR",1,now);
  }
  else if(co2Urgent.active)warningPage("CO2 ALERT",co2Raw,"ppm - HIGH CO2","LEAVE / GET FRESH AIR",1,now);
  else if(co2Attention.active)warningPage("HIGH CO2",co2Raw,"ppm","VENTILATE NOW",1,now);
  else if(issue) {
    // Rotate all active problems, keeping the problem text continuously visible.
    int active[5],n=0;
    if(co2Warning.active)active[n++]=0;
    if(pmWarning.active)active[n++]=1;
    if(vocWarning.active)active[n++]=2;
    if(fault)active[n++]=4;
    // An empty alert list must not become a divide-by-zero/out-of-range read.
    int which=n>0 ? active[(now/5000)%n] : -1;
    if(which==0)warningPage(co2Raw>=CO2_VENTILATE?"CO2 HIGH":"CO2 LOWER",co2Raw,"ppm","IMPROVE VENTILATION",1,now);
    if(which==1) {
      bool fine=pm25Smooth.value/PM25_ADVISORY>=pm10Smooth.value/PM10_ADVISORY;
      pmWarningPage(fine,fine?pm[1]:pm[3],false,
                    !particlesElevated(pm25Smooth.value,pm10Smooth.value));
    }
    if(which==2)warningPage(ensUBA>=4?"VOC HIGH":"VOC LOWER",voc.filtered.value,"ppb - FILTERED","REMOVE SOURCE / AIR",1,now);
    if(which==4)drawFault(now);
  }
  else if(warming)drawWarmup(now);
  else if(recoveryMs && !elapsed(now,recoveryMs,5000)) {centered("IMPROVED",5,2);checkAnimation(now);centered("MONITORING CONTINUES",112,1);}
  else if(modeNoticeMs && !elapsed(now,modeNoticeMs,1500)) {centered("Air-Lume",18,2);centered(modeName[mode],62,1);centered("MONITORING CONTINUES",110,1);}
  else drawCycle(now);
  // Snapshot a frame and transfer just one 128-byte page per loop. A blocking
  // 2KB display() at 100kHz would stall the fireplace for about 200ms.
  memcpy(oledFrame,oled.getBuffer(),sizeof(oledFrame));oledPage=0;
}
void serviceOLEDTransfer() {
  if(outputsOff){oledPage=-1;return;}
  if(oledPage<0 || !screenOK)return;
  if(oledPage>=16){screenOK=false;oledPage=-1;return;}
  // SH1107 128x128 page addressing, column offset 0; keep shared bus at 100kHz.
  Wire.beginTransmission(OLED_ADDR);
  Wire.write(0x00);Wire.write(0xB0+oledPage);Wire.write(0x10);Wire.write(0x00);
  bool ok=Wire.endTransmission()==0;
  for(int offset=0;ok && offset<128;offset+=32) {
    Wire.beginTransmission(OLED_ADDR);Wire.write(0x40);
    Wire.write(oledFrame+oledPage*128+offset,32);
    ok=Wire.endTransmission()==0;
  }
  if(!ok){screenOK=false;oledPage=-1;return;}
  if(++oledPage==16)oledPage=-1;
}

// ---- Outputs-off standby: ESP32, USB, Serial and button remain running ----
void clearMeasurementState(uint32_t now) {
  ensSeen=vocSeen=scdSeen=ahtSeen=spsSeen=0;
  co2Raw=tvocRaw=eco2=0;ensUBA=0;ensValidity=3;
  scdT=scdRH=ahtT=ahtRH=tempAvg=rhAvg=particleSize=NAN;
  for(float &x:pm)x=NAN;
  for(float &x:counts)x=NAN;
  co2Smooth=pm25Smooth=pm10Smooth=tempSmooth=rhSmooth=Filter{};
  voc.invalidate();
  co2Warning=co2Attention=co2Urgent=pmWarning=vocWarning=comfortWarning=Latch{};
  air=AirSignal{};
  fault=issue=co2Fresh=pmFresh=vocFresh=thFresh=false;
  recoveryMs=modeNoticeMs=0;
  startMs=ensStartMs=pageEpoch=now;warming=true;
}
// Presentation-only smoothing. Sensor data and alerts stay independent.
Filter serialFilters[6];
float serialHeld[6] = {NAN,NAN,NAN,NAN,NAN,NAN};
void serialMetric(int slot, const char *label, float value, uint32_t now,
                  float tau, float step, int decimals, const char *unit) {
  Serial.printf("  %-13s : ",label);
  if (!std::isfinite(value)) {
    serialFilters[slot]=Filter{};serialHeld[slot]=NAN;
    Serial.println("-- (waiting for sensor)");return;
  }
  serialFilters[slot].add(value,now,tau);
  float v=serialFilters[slot].value;
  // A small deadband stops values flickering across a rounding boundary.
  if (!std::isfinite(serialHeld[slot]) || std::fabs(v-serialHeld[slot])>=step*0.7f)
    serialHeld[slot]=std::round(v/step)*step;
  Serial.printf("%7.*f %s\n",decimals,serialHeld[slot],unit);
}
void printRawReadings() {
  Serial.println("RAW DIAGNOSTICS (unsmoothed; -- means unavailable)");
  Serial.printf("  SCD40: %s | AHT: %s | SPS30: %s | ENS validity: %u\n",
    co2Fresh?"fresh":"waiting",fresh(ahtSeen,millis(),15000)?"fresh":"waiting",
    pmFresh?"fresh":"waiting",ensValidity);
  if(co2Fresh)Serial.printf("  CO2=%u ppm\n",co2Raw);
  if(pmFresh)Serial.printf("  PM2.5=%.2f ug/m3 | PM10=%.2f ug/m3\n",pm[1],pm[3]);
  if(thFresh)Serial.printf("  Temperature=%.2f F (corrected) | Humidity=%.2f %% RH\n",fahrenheit(tempAvg),rhAvg);
  if(vocFresh)Serial.printf("  TVOC=%u ppb | VOC-based eCO2=%u ppm (estimate)\n",tvocRaw,eco2);
  Serial.printf("  Ambient sensor: removed | OLED=%s\n",screenOK?"OK":"unavailable");
  Serial.printf("  Strip: GPIO%d | %d RGB pixels | brightness target %u/255 | estimated budget %.0f mA\n",
    LED_PIN,LED_COUNT,mode==NIGHT?NIGHT_BRIGHTNESS:LED_BRIGHTNESS,LED_BUDGET_MA);
}
void printStatus() {
  uint32_t now=millis();
  Serial.println();
  Serial.println("+------------------------------------------------+");
  Serial.printf("  AIRLUME  |  %02lu:%02lu:%02lu since start\n",
    (unsigned long)(now/3600000),(unsigned long)(now/60000%60),(unsigned long)(now/1000%60));
  Serial.printf("  Lamp: %s  |  Mode: %s\n",outputsOff?"OFF":"ON",modeName[mode]);
  Serial.printf("  WiFi: OFF | OLED rotation: %u | T/RH: info only\n",OLED_ROTATION);
  Serial.printf("  Brightness target: %u/255 | strip budget: %.0f mA estimated\n",
      mode==NIGHT?NIGHT_BRIGHTNESS:LED_BRIGHTNESS,LED_BUDGET_MA);
  Serial.println("+------------------------------------------------+");
  if(sensorsPaused) {
    for(int i=0;i<6;++i){serialFilters[i]=Filter{};serialHeld[i]=NAN;}
    Serial.println("  Sensors paused. Send ON to resume.");
  } else {
    serialMetric(0,"CO2",co2Fresh?float(co2Raw):NAN,now,25,10,0,"ppm");
    serialMetric(1,"PM2.5",pmFresh?pm[1]:NAN,now,15,0.5f,1,"ug/m3");
    serialMetric(2,"PM10",pmFresh?pm[3]:NAN,now,15,0.5f,1,"ug/m3");
    serialMetric(3,"Temperature",thFresh?fahrenheit(tempAvg):NAN,now,20,0.5f,1,"F");
    serialMetric(4,"Humidity",thFresh?rhAvg:NAN,now,20,1,0,"% RH");
    serialMetric(5,"VOC",vocFresh?voc.filtered.value:NAN,now,10,10,0,"ppb (filtered estimate)");
    Serial.println("  ------------------------------------------------");
    Serial.println(air.high?"  Air indication: RED / high live reading.":(air.elevated?"  Air indication: YELLOW / elevated.":"  Air indication: normal on available channels."));
    if(co2Urgent.active)Serial.println("  CO2 alert: leave / get fresh air.");
    else if(co2Attention.active)Serial.println("  CO2 is high: ventilate now.");
    else if(co2Warning.active)Serial.println("  CO2 is elevated: improve ventilation.");
    else if(co2Fresh && co2Raw>=1500)Serial.println("  CO2 is rising/high: improve ventilation.");
    if(pmWarning.active)Serial.println("  Live PM advisory: remove source / filter air (not a 24h WHO assessment).");
    if(vocWarning.active)Serial.println("  VOC elevated: remove source / ventilate.");
    if(ensConditioning(now))Serial.println(ensValidity==2?
      "  VOC: initial conditioning (may take ~1 hour).":
      "  VOC: warming up (usually ~3 minutes).");
    else if(!vocFresh)Serial.println("  VOC: waiting for a valid reading.");
    if(!co2Fresh)Serial.println("  SCD40: waiting for fresh CO2 data.");
    if(!fresh(ahtSeen,now,15000))Serial.println("  AHT: waiting for temperature / humidity data.");
    if(!pmFresh)Serial.println("  SPS30: waiting for fresh particle data.");
    if(!screenOK)Serial.println("  OLED: not responding; check wiring / power.");
    if(!issue && !warming && co2Fresh && pmFresh)
      Serial.println(vocFresh?"  Monitoring: no active alerts.":"  CO2 + PM monitoring active; VOC joins when ready.");
    Serial.printf("  Temperature: room-reference offset %.1f F applied.\n",SCD_T_CORRECTION*1.8f);
    Serial.println("  Rounded + smoothed; real changes still show.");
  }
  Serial.println("+------------------------------------------------+");
}
void setOutputsOff(bool off) {
  if(outputsOff==off)return;
  outputsOff=off;
  oledPage=-1; // Cancel a partially transmitted old frame before blanking.
  memset(oledFrame,0,sizeof(oledFrame));
  if(screenOK && oled.getBuffer()==nullptr)screenOK=false;
  if(off) {
    pixels.clear();pixels.show(); // Actually transmit black to all 100 pixels.
    for(RGB &p:rendered)p=RGB{};
    for(float &h:heat)h=0;
    if(screenOK) {
      oled.clearDisplay();oled.display(); // Clear display RAM, then disable panel.
      oled.oled_command(SH110X_DISPLAYOFF);
    }
    if(PAUSE_SENSORS_WHEN_OFF) {
      // Only these peripheral sensors enter standby; the ESP32 never sleeps.
      if(scdOnline) {
        int16_t e=scd.stopPeriodicMeasurement();delay(500);
        if(e)Serial.printf("SCD40 stop failed: %d\n",int(e));
      }
      if(spsOnline) {
        int16_t e=sps.stopMeasurement();delay(20);
        if(e)Serial.printf("SPS30 stop failed: %d\n",int(e));
        e=sps.sleep();
        if(e)Serial.printf("SPS30 standby failed: %d\n",int(e));
      }
      if(ensOnline && !ensMode(0x01))Serial.println("ENS160 idle command failed");
      sensorsPaused=true;
      clearMeasurementState(millis());
    }
  } else {
    if(sensorsPaused) {
      clearMeasurementState(millis());
      initSCD();initSPS();ensOnline=initENS();
      sensorsPaused=false;
    }
    if(!screenOK) {
      screenOK=oled.begin(OLED_ADDR,true) && oled.getBuffer()!=nullptr;
      if(screenOK){oled.setRotation(OLED_ROTATION);oled.cp437(true);}
    }
    if(screenOK) {
      oled.clearDisplay();oled.display();oled.oled_command(SH110X_DISPLAYON);
    }
    pageEpoch=millis();
  }
  printStatus();
}
void serviceButton(uint32_t now) {
  switch(topButton.update(digitalRead(BUTTON_PIN)==LOW,outputsOff,now)) {
    case ButtonAction::OutputsOff:setOutputsOff(true);break;
    case ButtonAction::Restore:setOutputsOff(false);break;
    case ButtonAction::NextMode:mode=Mode((int(mode)+1)%MODE_COUNT);modeNoticeMs=now;break;
    default:break;
  }
}
void serviceSerial() {
  static char command[24];static uint8_t used=0;static bool overflow=false;
  for(int budget=0;budget<64 && Serial.available();++budget) {
    char c=Serial.read();
    if(c=='\r' || c=='\n') {
      if(used && !overflow) {
        command[used]='\0';
        if(strcmp(command,"OFF")==0)setOutputsOff(true);
        else if(strcmp(command,"ON")==0)setOutputsOff(false);
        else if(strcmp(command,"STATUS")==0)printStatus();
        else if(strcmp(command,"RAW")==0)printRawReadings();
        else if(strcmp(command,"WARM")==0){mode=WARM_WHITE;modeNoticeMs=millis();printStatus();}
        else if(strcmp(command,"COOL")==0 || strcmp(command,"WHITE")==0){mode=COOL_WHITE;modeNoticeMs=millis();printStatus();}
        else if(strcmp(command,"FIRE")==0){mode=FIRE;modeNoticeMs=millis();printStatus();}
        else if(strcmp(command,"NIGHT")==0){mode=NIGHT;modeNoticeMs=millis();printStatus();}
        else if(strcmp(command,"MONITOR")==0){mode=MONITOR;modeNoticeMs=millis();printStatus();}
        else Serial.println("Commands: ON, OFF, STATUS, RAW, WARM, WHITE, FIRE, NIGHT, MONITOR (newline)");
      }
      used=0;overflow=false;
    } else if(!overflow) {
      if(c>='a' && c<='z')c-=('a'-'A');
      if(used<sizeof(command)-1)command[used++]=c;
      else overflow=true;
    }
  }
}
void setup() {
  Serial.begin(115200); // Never wait for Serial: standalone operation must work.
  pinMode(BUTTON_PIN,INPUT_PULLUP);
  turnOffWiFi();
  if(!setCpuFrequencyMhz(CPU_MHZ))Serial.println("CPU frequency unchanged; continuing.");
  if(!pixels.begin() || pixels.getPixels()==nullptr)Serial.println("NeoPixel allocation/init failed; continuing without LEDs.");
  pixels.clear();pixels.show();
  Wire.begin(SDA_PIN,SCL_PIN,100000);Wire.setTimeOut(40);
  scd.begin(Wire,0x62);sps.begin(Wire,0x69);
  screenOK=oled.begin(OLED_ADDR,true) && oled.getBuffer()!=nullptr;
  startMs=millis();ensStartMs=startMs;pageEpoch=startMs;
  if(screenOK){oled.setRotation(OLED_ROTATION);oled.cp437(true);oled.clearDisplay();oled.setTextColor(SH110X_WHITE);oled.setTextWrap(false);drawWarmup(startMs);oled.display();}
  ahtOnline=aht.begin(&Wire);
  initSCD();initSPS();ensOnline=initENS();

  randomSeed(esp_random());
  Serial.println("AIRLUME | Fast startup | Fahrenheit | pages every 5 seconds");
  Serial.println("VOC joins automatically on valid sensor status; no software hour wait.");
  Serial.println("Commands: ON / OFF / STATUS / RAW / WARM / WHITE / FIRE / NIGHT / MONITOR (newline)");
  printStatus();
}
void loop() {
  static bool firstLoop=TRACE_FIRST_LOOP;
  if(firstLoop)Serial.println("[loop] serial/button");
  serviceSerial();
  uint32_t now=millis();serviceButton(now);
  now=millis();
  if(firstLoop)Serial.println("[loop] sensors");
  if(!sensorsPaused)pollSensors(now);
  now=millis();
  serviceIntro(now);
  if(firstLoop)Serial.println("[loop] model");
  if(!sensorsPaused && elapsed(now,modelTick,1000)){modelTick=now;updateModel(now);}
  if(firstLoop)Serial.println("[loop] LEDs");
  renderLEDs(now);
  if(firstLoop)Serial.println("[loop] OLED render");
  serviceDisplayBrightness();
  renderOLED(now);
  if(firstLoop)Serial.println("[loop] OLED transfer");
  serviceOLEDTransfer();
  if(firstLoop){Serial.println("[loop] complete");firstLoop=false;}
  static uint32_t logTick=0;
  if(elapsed(now,logTick,5000)) {
    logTick=now;
    printStatus();
  }
  delay(outputsOff?20:2); // Yield to USB/RTOS; does not enter an ESP sleep mode.
}
