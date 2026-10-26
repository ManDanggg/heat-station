/*
  HackIoT 2026: Heat Safety + Smart Cooling Station
  Board: Elegoo UNO R3 (Arduino Uno)

  What it does
  - DHT11 reads indoor temperature + humidity, computes the heat index (NWS formula)
  - DS18B20 (or the kit thermistor) reads the "outdoor" temperature
  - PIR motion sensor: no cooling and no alarm when the room is empty
  - RGB LED + LCD show the risk level, active buzzer warns at dangerous levels
  - If cooling is needed and outside is cooler: servo opens a vent (no "AC")
    otherwise: fan motor (via L293D) runs, faster as the heat index rises
  - Sends one JSON line every 2 s over USB serial for the laptop dashboard
  - Accepts one-letter commands over serial (see handleCommand)

  Libraries (Arduino IDE > Tools > Manage Libraries):
  - "DHT sensor library" by Adafruit (install "Adafruit Unified Sensor" when asked)
  - "DallasTemperature" by Miles Burton (install "OneWire" when asked)
  - LiquidCrystal and Servo come with the IDE
*/

#include <DHT.h>
#include <LiquidCrystal.h>
#include <Servo.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <math.h>

// ---------------- Pins ----------------
const uint8_t PIN_DHT     = 2;
const uint8_t PIN_RGB_R   = 3;   // PWM
const uint8_t PIN_BUZZER  = 4;   // ACTIVE buzzer (the one with the sealed bottom)
const uint8_t PIN_RGB_G   = 5;   // PWM
const uint8_t PIN_RGB_B   = 6;   // PWM
const uint8_t PIN_LCD_RS  = 7;
const uint8_t PIN_LCD_E   = 8;
const uint8_t PIN_SERVO   = 9;
const uint8_t PIN_FAN_IN1 = 10;  // L293D pin 2 (L293D pin 7 goes to GND)
const uint8_t PIN_FAN_EN  = 11;  // L293D pin 1, PWM speed
const uint8_t PIN_LCD_D4  = 12;
const uint8_t PIN_LCD_D5  = 13;
const uint8_t PIN_OUTDOOR = A0;  // DS18B20 signal (or thermistor divider)
const uint8_t PIN_LCD_D6  = A1;
const uint8_t PIN_LCD_D7  = A2;
const uint8_t PIN_BUTTON  = A3;  // other leg to GND, uses internal pull-up
const uint8_t PIN_PIR     = A4;  // HC-SR501 OUT

// ---------------- Settings ----------------
// Flip these to match what is wired
const bool USE_DS18B20 = true;    // false = use the kit thermistor on A0 instead
const bool USE_PIR     = false;   // set true once the PIR is wired to A4

// Room counts as empty after this long with no motion
const unsigned long EMPTY_AFTER_DEMO_MS = 30000;    // 30 s for judging
const unsigned long EMPTY_AFTER_REAL_MS = 600000;   // 10 min for real use

// Heat index thresholds in F for Caution / Warning / Danger.
// Real values follow the National Weather Service chart (80 / 90 / 103).
// Demo values are low enough to trigger with your hand or breath at the judging table.
const float REAL_LEVELS[3] = {80.0, 90.0, 103.0};
const float DEMO_LEVELS[3] = {78.0, 84.0, 90.0};
bool demoMode = true;             // long-press the button (or send 'd') to switch

const float LEVEL_HYST     = 1.0; // F, stops the level flickering at a threshold
const float OUTDOOR_MARGIN = 3.0; // outside must be this much cooler to use the vent

const int VENT_CLOSED_DEG = 0;
const int VENT_OPEN_DEG   = 90;
const int FAN_MIN_PWM     = 150;  // below this the kit motor may stall
const int FAN_MAX_PWM     = 255;

const unsigned long READ_INTERVAL_MS = 2000;   // DHT11 can't go faster than ~1 Hz
const unsigned long SCREEN_MS        = 3000;   // LCD page rotation
const unsigned long MUTE_MS          = 300000; // button mutes the alarm for 5 min
const unsigned long LONG_PRESS_MS    = 1500;

// Kit thermistor (only if USE_DS18B20 is false): 10k NTC, wired 5V -> 10k resistor -> A0 -> thermistor -> GND
const float SERIES_R  = 10000.0;
const float NOMINAL_R = 10000.0;
const float NOMINAL_C = 25.0;
const float BETA      = 3950.0;

// ---------------- Objects ----------------
DHT dht(PIN_DHT, DHT11);
LiquidCrystal lcd(PIN_LCD_RS, PIN_LCD_E, PIN_LCD_D4, PIN_LCD_D5, PIN_LCD_D6, PIN_LCD_D7);
Servo vent;
OneWire oneWire(PIN_OUTDOOR);
DallasTemperature ds18b20(&oneWire);

// ---------------- State ----------------
enum Mode { MODE_AUTO, MODE_MANUAL };
Mode mode = MODE_AUTO;

float inF = NAN, humidity = NAN, heatIndexF = NAN, outF = NAN;
bool dhtOk = false, outOk = false;
bool occupied = true;
unsigned long lastMotion = 0;
uint8_t level = 0;               // 0 OK, 1 Caution, 2 Warning, 3 Danger
bool ventOpen = false;
int fanPwm = 0;
unsigned long muteUntil = 0;
bool servoAttached = false;
unsigned long servoMovedAt = 0;

unsigned long lastRead = 0, lastScreen = 0;
uint8_t screen = 0;

// button debounce
bool btnStable = HIGH, btnLastRaw = HIGH;
unsigned long btnChangedAt = 0, btnPressedAt = 0;
bool longPressFired = false;

const char* LEVEL_NAMES[4] = {"OK", "CAUTION", "WARNING", "DANGER"};

// ---------------- Helpers ----------------
bool isMuted() {
  return (long)(muteUntil - millis()) > 0;
}

float readThermistorF(bool &ok) {
  long sum = 0;
  for (uint8_t i = 0; i < 8; i++) sum += analogRead(PIN_OUTDOOR);
  float adc = sum / 8.0;
  ok = (adc > 10 && adc < 1013);          // unplugged or shorted reads at the rails
  if (!ok) return NAN;
  float r = SERIES_R * adc / (1023.0 - adc);
  float kelvin = 1.0 / (1.0 / (NOMINAL_C + 273.15) + log(r / NOMINAL_R) / BETA);
  return (kelvin - 273.15) * 9.0 / 5.0 + 32.0;
}

float readDs18b20F(bool &ok) {
  float c = ds18b20.getTempCByIndex(0);   // result of the request made last cycle
  ds18b20.requestTemperatures();           // start the next one (takes ~750 ms)
  ok = (c != DEVICE_DISCONNECTED_C && c > -50.0 && c < 85.0);
  return ok ? c * 9.0 / 5.0 + 32.0 : NAN;
}

float readOutdoorF(bool &ok) {
  return USE_DS18B20 ? readDs18b20F(ok) : readThermistorF(ok);
}

void updateOccupancy() {
  if (!USE_PIR) { occupied = true; return; }
  unsigned long now = millis();
  if (digitalRead(PIN_PIR) == HIGH) lastMotion = now;
  unsigned long limit = demoMode ? EMPTY_AFTER_DEMO_MS : EMPTY_AFTER_REAL_MS;
  occupied = (now - lastMotion) < limit;
}

uint8_t levelFor(float hi, uint8_t current) {
  const float* L = demoMode ? DEMO_LEVELS : REAL_LEVELS;
  uint8_t lvl = 0;
  for (uint8_t i = 0; i < 3; i++) {
    float th = L[i];
    if (i < current) th -= LEVEL_HYST;    // must cool past the threshold to drop a level
    if (hi >= th) lvl = i + 1;
  }
  return lvl;
}

void setVent(bool open, bool force) {
  if (open == ventOpen && !force) return;
  ventOpen = open;
  if (!servoAttached) { vent.attach(PIN_SERVO); servoAttached = true; }
  vent.write(open ? VENT_OPEN_DEG : VENT_CLOSED_DEG);
  servoMovedAt = millis();
}

void setFan(int pwm) {
  pwm = constrain(pwm, 0, 255);
  if (pwm > 0 && fanPwm == 0) {           // kick-start so the motor doesn't stall
    analogWrite(PIN_FAN_EN, 255);
    delay(150);
  }
  fanPwm = pwm;
  analogWrite(PIN_FAN_EN, fanPwm);
}

void setRgb(uint8_t r, uint8_t g, uint8_t b) {   // kit RGB LED is common cathode
  analogWrite(PIN_RGB_R, r);
  analogWrite(PIN_RGB_G, g);
  analogWrite(PIN_RGB_B, b);
}

// ---------------- Control logic ----------------
void decideCooling() {
  if (mode != MODE_AUTO || !dhtOk) return;

  if (level == 0 || !occupied) {          // nothing to do, or nobody to cool
    if (ventOpen) setVent(false, false);
    if (fanPwm) setFan(0);
    return;
  }

  // Keep the vent open a little longer once it's open (hysteresis)
  float margin = ventOpen ? OUTDOOR_MARGIN - 1.0 : OUTDOOR_MARGIN;
  bool outsideCooler = outOk && (outF < inF - margin);

  if (outsideCooler) {
    if (!ventOpen) setVent(true, false);
    if (fanPwm) setFan(0);
  } else {
    if (ventOpen) setVent(false, false);
    const float* L = demoMode ? DEMO_LEVELS : REAL_LEVELS;
    float t = (heatIndexF - L[0]) / (L[2] - L[0]);
    t = constrain(t, 0.0, 1.0);
    setFan(FAN_MIN_PWM + (int)(t * (FAN_MAX_PWM - FAN_MIN_PWM)));
  }
}

void updateOutputs() {
  unsigned long now = millis();

  // RGB LED
  if (!dhtOk) {
    setRgb((now / 500) % 2 ? 80 : 0, 0, (now / 500) % 2 ? 80 : 0); // purple blink = sensor error
  } else if (!occupied) {
    setRgb(0, 0, 40);                                            // dim blue = room empty
  } else {
    switch (level) {
      case 0: setRgb(0, 120, 0);   break;
      case 1: setRgb(255, 150, 0); break;
      case 2: setRgb(255, 50, 0);  break;
      case 3: setRgb((now / 250) % 2 ? 255 : 0, 0, 0); break;   // flashing red
    }
  }

  // Buzzer: Danger = repeating beep, Warning = short chirp every 30 s
  bool buzz = false;
  if (dhtOk && occupied && !isMuted()) {
    if (level == 3) buzz = (now % 800) < 150;
    else if (level == 2) buzz = (now % 30000) < 100;
  }
  digitalWrite(PIN_BUZZER, buzz ? HIGH : LOW);

  // Detach servo after it reaches position: stops jitter and saves battery
  if (servoAttached && now - servoMovedAt > 700) {
    vent.detach();
    servoAttached = false;
  }
}

// ---------------- LCD ----------------
void lcdLine(uint8_t row, const char* text) {
  char buf[17];
  snprintf(buf, sizeof(buf), "%-16s", text);
  lcd.setCursor(0, row);
  lcd.print(buf);
}

void updateLcd() {
  char l1[17], l2[17];

  if (!dhtOk) {
    lcdLine(0, "DHT11 error");
    lcdLine(1, "Check pin D2");
    return;
  }

  uint8_t pages = (level >= 2 && occupied) ? 3 : 2;
  screen = screen % pages;

  if (screen == 0) {
    char action[12];
    if (!occupied)       snprintf(action, sizeof(action), "Empty");
    else if (ventOpen)   snprintf(action, sizeof(action), "Vent");
    else if (fanPwm > 0) snprintf(action, sizeof(action), "Fan %d%%", (int)(fanPwm * 100L / 255));
    else                 snprintf(action, sizeof(action), "Idle");
    snprintf(l1, sizeof(l1), "Feels like %dF", (int)round(heatIndexF));
    snprintf(l2, sizeof(l2), "%-8s%s", LEVEL_NAMES[level], action);
  } else if (screen == 1) {
    if (outOk) snprintf(l1, sizeof(l1), "In %dF  Out %dF", (int)round(inF), (int)round(outF));
    else       snprintf(l1, sizeof(l1), "In %dF  Out --", (int)round(inF));
    snprintf(l2, sizeof(l2), "Hum %d%% %s%s", (int)round(humidity),
             mode == MODE_AUTO ? "AUTO" : "MAN",
             isMuted() ? " M" : (demoMode ? " D" : ""));
  } else {
    snprintf(l1, sizeof(l1), "DRINK WATER!");
    snprintf(l2, sizeof(l2), level == 3 ? "Find a cool spot" : "Take a break");
  }
  lcdLine(0, l1);
  lcdLine(1, l2);
}

// ---------------- Serial ----------------
void printNum(const char* key, float v, uint8_t digits, bool valid) {
  Serial.print('"'); Serial.print(key); Serial.print("\":");
  if (valid && !isnan(v)) Serial.print(v, digits); else Serial.print("null");
}

void sendStatus() {
  Serial.print('{');
  printNum("in", inF, 1, dhtOk);        Serial.print(',');
  printNum("hum", humidity, 0, dhtOk);  Serial.print(',');
  printNum("hi", heatIndexF, 1, dhtOk); Serial.print(',');
  printNum("out", outF, 1, outOk);      Serial.print(',');
  Serial.print("\"lvl\":");  Serial.print(level);
  Serial.print(",\"fan\":"); Serial.print((int)(fanPwm * 100L / 255));
  Serial.print(",\"vent\":"); Serial.print(ventOpen ? 1 : 0);
  Serial.print(",\"occ\":"); Serial.print(occupied ? 1 : 0);
  Serial.print(",\"pir\":"); Serial.print(USE_PIR ? 1 : 0);
  Serial.print(",\"mute\":"); Serial.print(isMuted() ? 1 : 0);
  Serial.print(",\"mode\":\""); Serial.print(mode == MODE_AUTO ? "auto" : "manual");
  Serial.print("\",\"demo\":"); Serial.print(demoMode ? 1 : 0);
  Serial.print(",\"err\":"); Serial.print(dhtOk ? 0 : 1);
  Serial.println('}');
}

// a = auto, f = run fan, v = open vent, x = all off, m = mute toggle, d = demo toggle
void handleCommand(char c) {
  switch (c) {
    case 'a': mode = MODE_AUTO; decideCooling(); break;
    case 'f': mode = MODE_MANUAL; setVent(false, false); setFan(FAN_MAX_PWM); break;
    case 'v': mode = MODE_MANUAL; setFan(0); setVent(true, false); break;
    case 'x': mode = MODE_MANUAL; setFan(0); setVent(false, false); break;
    case 'm': muteUntil = isMuted() ? millis() : millis() + MUTE_MS; break;
    case 'd': demoMode = !demoMode; level = levelFor(heatIndexF, 0); decideCooling(); break;
    default: return;
  }
  sendStatus();   // dashboard sees the change right away
}

// ---------------- Button ----------------
void readButton() {
  bool raw = digitalRead(PIN_BUTTON);
  unsigned long now = millis();
  if (raw != btnLastRaw) { btnChangedAt = now; btnLastRaw = raw; }
  if (now - btnChangedAt < 30) return;              // debounce

  if (raw != btnStable) {
    btnStable = raw;
    if (btnStable == LOW) {                         // pressed
      btnPressedAt = now;
      longPressFired = false;
    } else if (!longPressFired) {                   // released after a short press
      handleCommand('m');
    }
  }
  if (btnStable == LOW && !longPressFired && now - btnPressedAt > LONG_PRESS_MS) {
    longPressFired = true;
    handleCommand('d');
    lcdLine(0, demoMode ? "Demo thresholds" : "Real thresholds");
    lcdLine(1, demoMode ? "78/84/90F" : "80/90/103F");
    lastScreen = now;                               // keep this message up for a moment
  }
}

// ---------------- Setup / loop ----------------
void setup() {
  Serial.begin(9600);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_FAN_IN1, OUTPUT);
  pinMode(PIN_FAN_EN, OUTPUT);
  pinMode(PIN_RGB_R, OUTPUT);
  pinMode(PIN_RGB_G, OUTPUT);
  pinMode(PIN_RGB_B, OUTPUT);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  if (USE_PIR) pinMode(PIN_PIR, INPUT);

  digitalWrite(PIN_FAN_IN1, HIGH);   // one direction only, speed comes from PWM on EN
  analogWrite(PIN_FAN_EN, 0);

  dht.begin();
  if (USE_DS18B20) {
    ds18b20.begin();
    ds18b20.setWaitForConversion(false);   // don't freeze the loop while it measures
    ds18b20.requestTemperatures();
  }
  lcd.begin(16, 2);
  lcdLine(0, "Heat Station");
  lcdLine(1, "Starting up...");

  setVent(false, true);              // move the flap to closed on boot
  setRgb(0, 0, 120);
  delay(2000);                       // DHT11 needs a moment after power-up
  lastMotion = millis();             // start as occupied
}

void loop() {
  unsigned long now = millis();

  while (Serial.available()) handleCommand((char)Serial.read());
  readButton();
  updateOccupancy();

  if (now - lastRead >= READ_INTERVAL_MS) {
    lastRead = now;

    float t = dht.readTemperature(true);   // true = Fahrenheit
    float h = dht.readHumidity();
    dhtOk = !(isnan(t) || isnan(h));
    if (dhtOk) {
      inF = t;
      humidity = h;
      heatIndexF = dht.computeHeatIndex(inF, humidity, true);
      level = levelFor(heatIndexF, level);
    }
    outF = readOutdoorF(outOk);

    decideCooling();
    sendStatus();
  }

  if (now - lastScreen >= SCREEN_MS) {
    lastScreen = now;
    screen++;
    updateLcd();
  }

  updateOutputs();
}
