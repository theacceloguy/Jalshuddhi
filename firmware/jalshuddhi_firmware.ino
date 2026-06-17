/*
 * =============================================================================
 *  JALSHUDDHI  -  Open Source River-Water Purification & Monitoring System
 * =============================================================================
 *
 *  File        : jalshuddhi_firmware.ino
 *  Target      : ESP32 (ESP32-WROOM-32 DevKit, 38-pin)
 *  Framework   : Arduino-ESP32 (core >= 2.0.x)
 *  Version     : 1.0.0
 *  Author      : Sayaan Mehta
 *  Mentor      : Satyam Prakash
 *  License     : MIT  (see LICENSE file in repository root)
 *  SPDX-License-Identifier: MIT
 *
 *  PURPOSE
 *  -------
 *  This firmware runs the electronic monitoring-and-control subsystem of an
 *  inline, multi-stage water purification unit intended for households that
 *  rely on river / canal (surface) water in the absence of a piped supply.
 *
 *  It performs four jobs:
 *    1. Reads four water-quality parameters  ............  pH, TDS, turbidity,
 *                                                          temperature
 *    2. Temperature-compensates the pH and TDS readings.
 *    3. Drives the purification cycle through a non-blocking state machine
 *       (submersible pump -> filtration -> UV -> booster pump -> dispense).
 *    4. Applies safety limits: water is only released to the clean-water
 *       outlet when every parameter is inside a safe drinking-water window.
 *       Otherwise the unit holds and shows a warning.
 *
 *  DESIGN NOTES
 *  ------------
 *  * The main loop never calls delay(). All timing is millis()-based so the
 *    UI, sensors and safety checks keep running while pumps are active.
 *  * Every analog parameter is read as a MEDIAN of N samples to reject the
 *    electrical spikes that are common with cheap analog water probes.
 *  * All calibration constants live in the CONFIG block below and are the
 *    only values a builder normally needs to change.
 *
 *  CALIBRATION (do this once per probe, before field use)
 *  ------------------------------------------------------
 *    pH        : measure two buffer solutions (pH 4.00 and pH 7.00), record
 *                the millivolts, and set PH_CAL_* below.
 *    TDS       : DFRobot factory calibration in a known ~707 ppm solution.
 *    Turbidity : clear water should read close to 0 NTU; adjust TURB_CLEAR_V.
 *    Temp      : DS18B20 is factory-calibrated, no action required.
 *
 *  THIRD-PARTY LIBRARIES (all open source, install via Library Manager)
 *  --------------------------------------------------------------------
 *    OneWire              by Paul Stoffregen        (MIT-style)
 *    DallasTemperature    by Miles Burton           (LGPL)
 *    LiquidCrystal_I2C    by Frank de Brabander      (MIT)
 *
 *  WIRING  (see hardware/wiring.md and the BOM for module part numbers)
 *  --------------------------------------------------------------------
 *    pH analog out      -> GPIO 35 (ADC1_CH7, input only)
 *    TDS analog out     -> GPIO 34 (ADC1_CH6, input only)
 *    Turbidity analog   -> GPIO 32 (ADC1_CH4)
 *    DS18B20 data       -> GPIO 33 (with 4.7k pull-up to 3V3)
 *    I2C LCD SDA / SCL  -> GPIO 21 / GPIO 22
 *    Start/Stop button  -> GPIO 18 (to GND, uses internal pull-up)
 *    Submersible pump   -> GPIO 25  (relay IN, active-LOW board assumed)
 *    Booster pump       -> GPIO 26  (relay IN)
 *    Solenoid valve     -> GPIO 27  (relay IN)
 *    UV lamp ballast    -> GPIO 14  (relay IN)
 *    Status LED         -> GPIO  2  (on-board LED)
 *
 *  SAFETY
 *  ------
 *  Mains-powered pumps, a UV lamp and water share one enclosure. The relay
 *  board MUST be opto-isolated, the UV lamp MUST be fully shrouded (UV-C is
 *  harmful to eyes/skin), and all mains wiring MUST be enclosed and fused.
 *  This firmware is provided "as is" without warranty (see MIT licence).
 * =============================================================================
 */

#include <OneWire.h>
#include <DallasTemperature.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

/* =====================  CONFIG  ============================================ */
/* ---- GPIO assignments ---------------------------------------------------- */
namespace Pin {
  constexpr uint8_t PH         = 35;   // ADC1 - input only
  constexpr uint8_t TDS        = 34;   // ADC1 - input only
  constexpr uint8_t TURBIDITY  = 32;   // ADC1
  constexpr uint8_t ONEWIRE    = 33;   // DS18B20
  constexpr uint8_t BUTTON     = 18;   // start/stop, INPUT_PULLUP
  constexpr uint8_t PUMP_SUB   = 25;   // submersible (intake) pump relay
  constexpr uint8_t PUMP_BOOST = 26;   // booster pump relay
  constexpr uint8_t VALVE      = 27;   // solenoid valve relay
  constexpr uint8_t UV_LAMP    = 14;   // UV lamp relay
  constexpr uint8_t STATUS_LED = 2;    // on-board LED
}

/* ---- Relay polarity ------------------------------------------------------ */
/* Most low-cost relay boards are ACTIVE-LOW (a LOW level energises the coil).
 * Set to false if your board is active-high.                                 */
constexpr bool RELAY_ACTIVE_LOW = true;

/* ---- ADC / sampling ------------------------------------------------------ */
constexpr float    ADC_VREF      = 3.30f;  // ESP32 ADC reference (volts)
constexpr int      ADC_MAX       = 4095;   // 12-bit ADC
constexpr uint8_t  SAMPLE_COUNT  = 21;     // samples per median read (odd)
constexpr uint16_t SAMPLE_GAP_MS = 4;      // gap between samples

/* ---- pH probe calibration (two-point) ------------------------------------
 * Record the probe voltage in two buffers, then enter them here.
 * Default values are typical for a DFRobot Gravity analog pH board.          */
constexpr float PH_CAL_V1 = 2.030f;  constexpr float PH_CAL_PH1 = 7.00f; // neutral
constexpr float PH_CAL_V2 = 2.540f;  constexpr float PH_CAL_PH2 = 4.00f; // acid

/* ---- TDS calibration ----------------------------------------------------- */
constexpr float TDS_K_FACTOR = 1.000f;  // tune in a known reference solution

/* ---- Turbidity calibration ----------------------------------------------- */
/* SEN0189 outputs ~4.1 V in clear water and the voltage DROPS as turbidity
 * rises. We map voltage -> NTU with a quadratic from the datasheet.          */
constexpr float TURB_CLEAR_V = 4.10f;   // voltage observed in clear water

/* ---- Safe drinking-water window (edit to match local standard, e.g. BIS) - */
constexpr float PH_MIN   = 6.5f,  PH_MAX   = 8.5f;    // BIS / WHO acceptable
constexpr float TDS_MAX  = 500.0f;                    // ppm, BIS acceptable
constexpr float TURB_MAX = 5.0f;                      // NTU, BIS permissible
constexpr float TEMP_MAX = 35.0f;                     // deg C, sanity limit

/* ---- Cycle timing (milliseconds) ----------------------------------------- */
constexpr uint32_t PRIME_MS       = 8000;   // intake pump primes the line
constexpr uint32_t FLUSH_MS       = 5000;   // initial flush to waste
constexpr uint32_t SENSOR_PERIOD  = 1000;   // how often to refresh readings
constexpr uint32_t LCD_PERIOD     = 2500;   // how often the LCD page rotates
constexpr uint32_t SERIAL_PERIOD  = 2000;   // telemetry interval

/* =====================  GLOBAL OBJECTS  =================================== */
OneWire           oneWire(Pin::ONEWIRE);
DallasTemperature tempSensor(&oneWire);
LiquidCrystal_I2C lcd(0x27, 16, 2);   // change 0x27 -> 0x3F if blank screen

/* Live measurement snapshot */
struct Reading {
  float ph          = 7.0f;
  float tds         = 0.0f;   // ppm
  float turbidity   = 0.0f;   // NTU
  float temperature = 25.0f;  // deg C
  bool  safe        = false;
} reading;

/* Purification state machine */
enum class State : uint8_t {
  IDLE,        // waiting for the start button
  PRIMING,     // intake pump fills the filter train
  FLUSHING,    // first-pass water sent to waste via solenoid
  RUNNING,     // booster + UV on, monitoring quality
  HOLD_UNSAFE, // quality outside limits: dispensing blocked
  FAULT        // sensor fault (e.g. temperature probe missing)
};
State state = State::IDLE;

/* Timing bookkeeping */
uint32_t stateEnteredAt = 0;
uint32_t lastSensorMs   = 0;
uint32_t lastLcdMs      = 0;
uint32_t lastSerialMs   = 0;
uint8_t  lcdPage        = 0;

/* Button debounce */
bool     lastBtnLevel   = HIGH;
uint32_t lastBtnChange  = 0;
constexpr uint32_t DEBOUNCE_MS = 40;

/* =====================  LOW-LEVEL HELPERS  ================================ */

/* Drive a relay channel respecting board polarity. */
inline void relay(uint8_t pin, bool on) {
  digitalWrite(pin, (on ^ RELAY_ACTIVE_LOW) ? HIGH : LOW);
}

/* Turn every actuator off - the safe default. */
void allActuatorsOff() {
  relay(Pin::PUMP_SUB,   false);
  relay(Pin::PUMP_BOOST, false);
  relay(Pin::VALVE,      false);
  relay(Pin::UV_LAMP,    false);
}

/* Read one ADC pin SAMPLE_COUNT times and return the median voltage.
 * Median (not mean) rejects the occasional wild spike from analog probes. */
float medianVoltage(uint8_t pin) {
  int buf[SAMPLE_COUNT];
  for (uint8_t i = 0; i < SAMPLE_COUNT; ++i) {
    buf[i] = analogRead(pin);
    delay(SAMPLE_GAP_MS);          // tiny, only inside the read helper
  }
  // insertion sort (SAMPLE_COUNT is small)
  for (uint8_t i = 1; i < SAMPLE_COUNT; ++i) {
    int key = buf[i]; int j = i - 1;
    while (j >= 0 && buf[j] > key) { buf[j + 1] = buf[j]; --j; }
    buf[j + 1] = key;
  }
  int raw = buf[SAMPLE_COUNT / 2];
  return (raw * ADC_VREF) / ADC_MAX;
}

/* =====================  SENSOR CONVERSIONS  ============================== */

/* pH: two-point linear fit, then compensate ~ -0.03 pH / degC drift. */
float readPH(float tempC) {
  float v = medianVoltage(Pin::PH);
  float slope = (PH_CAL_PH2 - PH_CAL_PH1) / (PH_CAL_V2 - PH_CAL_V1);
  float ph = PH_CAL_PH1 + slope * (v - PH_CAL_V1);
  ph += (25.0f - tempC) * 0.03f;            // temperature compensation
  if (ph < 0)  ph = 0;
  if (ph > 14) ph = 14;
  return ph;
}

/* TDS: DFRobot polynomial, with standard 2%/degC temperature compensation. */
float readTDS(float tempC) {
  float v = medianVoltage(Pin::TDS);
  float comp = v / (1.0f + 0.02f * (tempC - 25.0f));   // compensate to 25C
  float tds = (133.42f * comp * comp * comp
             - 255.86f * comp * comp
             + 857.39f * comp) * 0.5f * TDS_K_FACTOR;
  if (tds < 0) tds = 0;
  return tds;
}

/* Turbidity: SEN0189 voltage -> NTU (datasheet quadratic). Clear water ~0. */
float readTurbidity() {
  float v = medianVoltage(Pin::TURBIDITY);
  if (v > TURB_CLEAR_V) v = TURB_CLEAR_V;              // clamp to clear baseline
  float ntu = -1120.4f * v * v + 5742.3f * v - 4352.9f;
  if (ntu < 0)    ntu = 0;
  if (ntu > 3000) ntu = 3000;
  return ntu;
}

/* Refresh all four parameters and recompute the safety verdict. */
void updateReadings() {
  tempSensor.requestTemperatures();
  float t = tempSensor.getTempCByIndex(0);

  if (t <= -100.0f) {            // DS18B20 disconnected -> fault
    state = State::FAULT;
    return;
  }
  reading.temperature = t;
  reading.ph          = readPH(t);
  reading.tds         = readTDS(t);
  reading.turbidity   = readTurbidity();

  reading.safe = (reading.ph  >= PH_MIN  && reading.ph  <= PH_MAX) &&
                 (reading.tds  <= TDS_MAX) &&
                 (reading.turbidity <= TURB_MAX) &&
                 (reading.temperature <= TEMP_MAX);
}

/* =====================  UI: LCD + SERIAL  ================================ */

void lcdBanner(const char* l1, const char* l2) {
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(l1);
  lcd.setCursor(0, 1); lcd.print(l2);
}

/* Rotate through the live readings, two values per page. */
void lcdShowReadings() {
  char a[17], b[17];
  switch (lcdPage % 3) {
    case 0:
      snprintf(a, sizeof(a), "pH:%4.1f", reading.ph);
      snprintf(b, sizeof(b), "TDS:%4.0f ppm", reading.tds);
      break;
    case 1:
      snprintf(a, sizeof(a), "Turb:%5.1f NTU", reading.turbidity);
      snprintf(b, sizeof(b), "Temp:%5.1f C", reading.temperature);
      break;
    default:
      snprintf(a, sizeof(a), "Water quality:");
      snprintf(b, sizeof(b), "%s", reading.safe ? "SAFE - OK" : "UNSAFE-HOLD");
      break;
  }
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(a);
  lcd.setCursor(0, 1); lcd.print(b);
  ++lcdPage;
}

void serialTelemetry() {
  Serial.printf("[%-11s] pH=%.2f  TDS=%.0fppm  Turb=%.1fNTU  T=%.1fC  -> %s\n",
                stateName(state),
                reading.ph, reading.tds, reading.turbidity,
                reading.temperature, reading.safe ? "SAFE" : "UNSAFE");
}

const char* stateName(State s) {
  switch (s) {
    case State::IDLE:        return "IDLE";
    case State::PRIMING:     return "PRIMING";
    case State::FLUSHING:    return "FLUSHING";
    case State::RUNNING:     return "RUNNING";
    case State::HOLD_UNSAFE: return "HOLD_UNSAFE";
    case State::FAULT:       return "FAULT";
  }
  return "?";
}

/* =====================  STATE MACHINE  ================================== */

void enterState(State s) {
  state = s;
  stateEnteredAt = millis();

  switch (s) {
    case State::IDLE:
      allActuatorsOff();
      lcdBanner("JALSHUDDHI v1.0", "Press to START");
      break;
    case State::PRIMING:
      allActuatorsOff();
      relay(Pin::PUMP_SUB, true);          // draw raw water in
      lcdBanner("Priming intake", "please wait...");
      break;
    case State::FLUSHING:
      relay(Pin::PUMP_SUB, true);
      relay(Pin::VALVE,    true);          // send first water to waste
      lcdBanner("Flushing line", "to waste...");
      break;
    case State::RUNNING:
      relay(Pin::PUMP_SUB,   true);
      relay(Pin::PUMP_BOOST, true);        // push through UF membrane
      relay(Pin::UV_LAMP,    true);        // disinfect
      relay(Pin::VALVE,      false);       // valve closed -> clean outlet
      break;
    case State::HOLD_UNSAFE:
      relay(Pin::PUMP_BOOST, false);       // stop pushing to clean outlet
      relay(Pin::UV_LAMP,    true);        // keep disinfecting
      relay(Pin::VALVE,      true);        // divert to waste
      lcdBanner("UNSAFE WATER", "Dispense halted");
      break;
    case State::FAULT:
      allActuatorsOff();
      lcdBanner("SENSOR FAULT", "Check temp probe");
      break;
  }
}

/* Returns true on a clean button press (debounced, falling edge). */
bool buttonPressed() {
  bool level = digitalRead(Pin::BUTTON);
  uint32_t now = millis();
  if (level != lastBtnLevel && (now - lastBtnChange) > DEBOUNCE_MS) {
    lastBtnChange = now;
    lastBtnLevel  = level;
    if (level == LOW) return true;         // pressed (pull-up -> active low)
  }
  return false;
}

/* =====================  ARDUINO SETUP / LOOP  =========================== */

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println(F("\nJALSHUDDHI firmware v1.0.0 starting..."));

  pinMode(Pin::BUTTON, INPUT_PULLUP);
  pinMode(Pin::STATUS_LED, OUTPUT);
  pinMode(Pin::PUMP_SUB,   OUTPUT);
  pinMode(Pin::PUMP_BOOST, OUTPUT);
  pinMode(Pin::VALVE,      OUTPUT);
  pinMode(Pin::UV_LAMP,    OUTPUT);

  allActuatorsOff();                       // fail-safe: everything off

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);          // full 0-3.3V range

  tempSensor.begin();
  Wire.begin(Pin::STATUS_LED == 21 ? 4 : 21, 22); // SDA=21, SCL=22
  lcd.init();
  lcd.backlight();

  enterState(State::IDLE);
}

void loop() {
  uint32_t now = millis();

  /* ---- always-on: refresh sensors on a fixed cadence ---- */
  if (now - lastSensorMs >= SENSOR_PERIOD) {
    lastSensorMs = now;
    if (state != State::FAULT) updateReadings();
    digitalWrite(Pin::STATUS_LED, reading.safe);   // LED = "safe to drink"
  }

  /* ---- always-on: serial telemetry ---- */
  if (now - lastSerialMs >= SERIAL_PERIOD) {
    lastSerialMs = now;
    serialTelemetry();
  }

  /* ---- state-specific behaviour ---- */
  switch (state) {

    case State::IDLE:
      if (buttonPressed()) enterState(State::PRIMING);
      break;

    case State::PRIMING:
      if (buttonPressed())                      { enterState(State::IDLE); break; }
      if (now - stateEnteredAt >= PRIME_MS)       enterState(State::FLUSHING);
      break;

    case State::FLUSHING:
      if (buttonPressed())                      { enterState(State::IDLE); break; }
      if (now - stateEnteredAt >= FLUSH_MS)       enterState(State::RUNNING);
      break;

    case State::RUNNING:
      if (buttonPressed())                      { enterState(State::IDLE); break; }
      if (!reading.safe)                          enterState(State::HOLD_UNSAFE);
      else if (now - lastLcdMs >= LCD_PERIOD)   { lastLcdMs = now; lcdShowReadings(); }
      break;

    case State::HOLD_UNSAFE:
      if (buttonPressed())                      { enterState(State::IDLE); break; }
      if (reading.safe)                           enterState(State::RUNNING);
      else if (now - lastLcdMs >= LCD_PERIOD)   { lastLcdMs = now; lcdShowReadings(); }
      break;

    case State::FAULT:
      if (buttonPressed()) {                    // allow a retry after a fix
        updateReadings();
        if (state == State::FAULT) enterState(State::FAULT); // still faulty
        else                       enterState(State::IDLE);
      }
      break;
  }
}
