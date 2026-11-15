# AirLume - The One Stop Solution for Your Air Quality Measurement Solutions

The active `AirLumeSerial` sketch addresses **200 pixels** and applies a **1,000 mA estimated strip-current budget**, with 150/255 day / 8/255 Night brightness targets. The user reports that a separate 5 V strip supply is connected and approved uploading this configuration. It was uploaded on November 15, 2026; flash verification passed, and live serial diagnostics reported GPIO2, 200 RGB pixels and the 1,000 mA estimated budget. The PM warning layout now shows a full-width PM heading, readable status and value, the yellow and red thresholds in text, and a bar with its marker at the red threshold. A prior restart reading was 75.42 F while settling; the single-point offset is not proof of completed calibration.

The older uncapped 200-pixel draft at `proposed/AirLume200/AirLume200.ino` was **not uploaded**. It remains for reference and does not contain the 1 A budget or the latest PM warning layout.

The details below describe the active, current-budgeted configuration.

# AirLume

Open `AirLumeSerial/AirLumeSerial.ino` in Arduino IDE. Target: ESP32C6 Dev Module with USB CDC On Boot enabled. Serial Monitor: 115200 baud, New Line.

## Offline firmware

The active firmware disables Wi-Fi with `esp_wifi_stop` / `esp_wifi_deinit`. It does not create an access point, join a network or run a web server. The former dashboard/daily-model headers remain in the folder as inactive source; they are not included by this build.

## Startup and controls

At power-on the OLED (rotation **0**) shows **AirLume** bold and centered, with “Initializing...” and rotating dots, for **3 seconds**. The main display then cycles every **5 seconds** through CO2, Fahrenheit temperature, humidity, PM2.5, PM10, valid VOC and lamp mode. Each numeric page uses consistent typography and a bottom comparison bar with a reference marker. A separate thin line shows page dwell time. Missing values show `--`; VOC is omitted until valid.

CO2/PM bars mark their configured warning/reference concentrations. PM warning pages instead mark the configured red threshold, print both yellow and red thresholds, and show the status in full, with no clipped “PM ADVISORY” heading. The VOC bar uses the sensor's AQI (marker 4/5), not an invented universal ppb danger threshold. The lamp-mode bar shows the five available modes; there is no overall score. Temperature's marker is an informational **80 F comfort reference**; humidity marks **40–60%** as a comfort band. Neither temperature nor humidity causes red, changes lamp color, or produces a warning page. All temperature output is Fahrenheit, with the current -7.888889 C correction applied once.

The ENS160 itself reports initial conditioning (up to one hour) and warmup (typically three minutes). Do not reinterpret conditioning data as valid VOC measurements. Conditioning status is stored after 24 continuous hours, so early power cycling can repeat it. Other sensors, alerts and lamp controls continue throughout.

Green/Red air monitoring is the **power-on mode**. A short button tap cycles **Green/Red → Warm White → White → Fire → Night → Green/Red**; these are the only five modes. Serial commands `MONITOR`, `WARM`, `WHITE` (or `COOL`), `FIRE`, and `NIGHT` select them directly, with a fast transition. Green/Red uses green in normal monitored conditions and yellow for elevated CO2/PM/VOC and red for high live readings; missing pollutant data uses blue and hardware faults use amber. `ON` / `OFF` switch the visual outputs; sensing continues even while outputs are off. An explicit OFF keeps the visual outputs dark, including alerts, while the serial report and monitoring continue. Elevated CO2/PM/VOC readings override enabled lamp modes with yellow shades, and high readings override them with red; sensor faults use amber. Only high-reading red indications pulse; a 2500 ppm ventilation reminder remains yellow. The 1 A estimated current budget may scale LED channels below the brightness target.

`STATUS` prints a readable report. `RAW` prints diagnostics and the configured data pin. Main serial reports update every 5 seconds using rounded/smoothed values; alerts use independent inputs. Presentation increments: 10 ppm CO2, 0.5 ug/m3 PM, 0.5 F temperature, 1% RH, 10 ppb VOC. Display increments do not imply sensor accuracy.

## Threshold policy

WHO references below have averaging periods. Live advisory thresholds derived from them are **product settings**, not proof of WHO compliance, an instantaneous health boundary, or regulatory AQI.

| Channel | Warning trigger | Clear condition | Basis / meaning |
|---|---|---|---|
| Measured SCD40 CO2 | >=1500 ppm for 30 s of fresh samples | <1300 for 60 s | Ventilation reminder, based on UK guidance for consistently elevated CO2; not a WHO toxicity threshold. 1200 ppm produces no CO2 warning or CO2 score contribution. |
| CO2 attention | >=2500 ppm for 10 s | <2000 for 60 s | Product ventilation reminder; yellow indication. |
| CO2 high | >=5000 ppm, first fresh sample | <4000 for 60 s | Product high-reading precaution, red pulse; not an instantaneous occupational exposure limit. |
| Live PM advisory | Smoothed PM2.5 >15 OR PM10 >45 ug/m3 continuously for 60 s | Both PM2.5 <12 and PM10 <36 for 60 s | WHO references are **24-hour means** of 15 / 45; this 60-second live advisory is not a WHO exposure assessment. WHO annual means are 5 / 15. |
| VOC estimate | Valid ENS160 AQI-UBA >=4 for 30 s | AQI <=2 for 60 s | ENS160 manufacturer's poor/unhealthy categories. No generic WHO TVOC-ppb limit is asserted. Display smoothing does not delay this AQI alert. The former 90-second VOC spike suppression is removed. |
| Temperature / humidity | No alert or score contribution | — | Diagnostics and ENS160 compensation only; excluded from air-quality decisions by request. |
| PM1/PM4, particle counts, particle size, light ADC, VOC-based eCO2 | No independent alert | — | Diagnostics; no invented WHO limits. eCO2 is not measured CO2. |

Dwell avoids reacting to isolated noisy samples; it is not a published WHO averaging period. Alerts clear only after the corresponding hysteresis/dwell condition. Missing/stale sensors produce a fault, not an all-clear. This prototype is not a certified smoke, CO or life-safety alarm.

The perceived/overall AirLume score has been removed from both calculations and the display. Temperature and humidity do not affect the air indication.

### Green / yellow / red indication

| Channel | Yellow / elevated | Red / high live reading |
|---|---|---|
| Measured CO2 | >=1500 ppm | >=5000 ppm |
| PM2.5 | >15 ug/m3 | >=55.5 ug/m3 |
| PM10 | >45 ug/m3 | >=255 ug/m3 |
| Valid ENS160 AQI | 3–4 | 5 |

Yellow blends from pale yellow toward saturated yellow as readings rise. A high fresh reading changes the light red and shows the corresponding OLED warning on the next model refresh (about one second); it does not wait for the ordinary advisory dwell. A retained high-CO2 latch can keep red active until its clear condition is met. Ordinary advisory latches keep yellow during recovery. Lamp modes and Night retain their brightness targets while showing these colors.

These are **product live-reading indications**, not a statement that lower concentrations are safe or that a higher sample proves immediate danger. The PM red concentrations correspond to EPA's **24-hour** unhealthy-category lower breakpoints; using those numbers for a live red indication is a product precaution, not calculated daily AQI or an instantaneous exposure assessment. WHO PM references also have 24-hour averaging periods. A generic TVOC instantaneous health limit is not asserted.

Source: [EPA AQI breakpoint table](https://aqs.epa.gov/aqsweb/documents/codetables/aqi_breakpoints.html).

## Temperature correction

Both temperature sensors have a provisional **-7.888889 C (-14.2 F)** total additive correction. This includes the prior -3 C offset plus an additional -8.8 F adjustment from the user's latest room comparison: a displayed **81.8 F** versus a **73 F** room reference. The offset is applied exactly once at measurement ingestion, then converted to Fahrenheit for output. It is a one-point room adjustment, not a laboratory calibration; sensor self-heating may change with runtime/enclosure conditions. Temperature and humidity do not affect the lamp color or air-quality alerts.

## Strip wiring and power

User-confirmed strip type: BTF 5V WS2812B, DIN on ESP32 GPIO2. Configured for **200 RGB pixels**, GRB / 800kHz, with a **1,000 mA estimated software budget for the whole strip**. Day modes target **150/255**; Night targets **8/255**, with OLED contrast also dimmed to 8 (day 150). Channel values are scaled as needed to fit the estimated budget. The ambient sensor is removed and no ADC light pin is read. Warm White and Night use **255/165/65 RGB**, a visibly golden approximation of 2700 K. White uses **255/200/140 RGB**, the old Warm White mix that the user observed as regular white. Confirmed pollution alerts still turn either mode red at its brightness target. Exact color temperature cannot be specified for this RGB-only strip without optical calibration.

The user now reports a separate 5 V supply connected directly to the strip, with common ESP32 ground. The ESP32 can remain USB powered. The 1 A limit is calculated from a nominal per-pixel model (1 mA idle plus 20 mA per full-scale RGB channel); it does not measure actual current or replace a supply and wiring rated for the strip load.

PM headings read **PM 2.5** and **PM 10** at text size 3. PM warning pages use the same large heading and value, show **ELEVATED**, **HIGH READING**, or **FALLING** in full, and mark the red threshold on the bottom bar.

## Validation

The unused daily-model source and `tests/test_daily.py` are retained for reference; they do not run on the device in this offline build.

The complete firmware compiles for ESP32C6 Dev Module / USB CDC enabled. Run `python3 tests/test_monitor.py` to exercise actual firmware model, page selection and LED renderer against host peripheral stubs. Tests cover startup without VOC, immediate valid-VOC activation, five-second page timing, omission/insertion of VOC pages, CO2 policy, Fahrenheit conversion, particle thresholds, exclusion of temperature/humidity from decisions, and red override in all lamp modes. Checks also cover the three-second intro, rotation 0, brightness target 150/255, disabled Wi-Fi, no ambient ADC reads, five modes, Night contrast 8/255, all 200 addressed pixels, the estimated 1 A limit, and lower Night output. PM warning string widths are checked against the 128-pixel OLED. Physical OLED/strip appearance still needs observation on the device.

The untouched original source is backed up in `original/`. Updated source lives in `AirLumeSerial/`; do not re-upload the older Downloads sketch.

## Sources reviewed November 14–15, 2026

- [ScioSense ENS160 datasheet v1.3, sections 5, 10, 11](https://www.sciosense.com/wp-content/uploads/2023/12/ENS160-Datasheet.pdf): sensor validity, conditioning, warmup and AQI-UBA.
- [WHO pollutant guideline table](https://www.who.int/teams/environment-climate-change-and-health/air-quality-and-health/health-impacts/types-of-pollutants): PM averaging periods and concentrations.
- [WHO selected indoor pollutants](https://www.who.int/publications/i/item/9789289002134): chemical-specific guidance; does not supply a generic TVOC-ppb alarm.
- [WHO Housing and Health Guidelines](https://iris.who.int/bitstream/handle/10665/276001/9789241550376-eng.pdf): cold-season indoor temperature recommendation; vulnerable people may need warmer conditions.
- [Welsh Government CO2 monitor guidance](https://www.gov.wales/carbon-dioxide-monitors-education-settings): consistently over 1500 ppm and ventilation.
- [EPA moisture guidance](https://www.epa.gov/mold/brief-guide-mold-moisture-and-your-home): humidity below 60%, ideally 30–50%.
- [Adafruit NeoPixel powering guidance](https://learn.adafruit.com/adafruit-neopixel-uberguide/powering-neopixels): power planning and common ground.
- [WLED color-temperature handling](https://kno.wled.ge/interfaces/json-api/): RGB color-temperature values are approximations without known LED spectra or white channels.
