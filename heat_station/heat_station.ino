/*
  Heat Station
  Built at IEEE HackIoT 2026 (USC) on an Elegoo UNO R3

  The idea: a room that knows how hot it actually feels, and cools itself
  the greenest way it can!

  Here's what it does:
  - The DHT11 reads indoor temperature and humidity, then we work out the
    heat index (the "feels like" temperature the National Weather Service uses)
  - The DS18B20 sits outside and tells us if it's cooler out there
  - An optional PIR motion sensor lets the station skip cooling empty rooms
  - The RGB LED and LCD show the risk level, and the buzzer speaks up when it
    gets dangerous
  - Need cooling and it's cooler outside? The servo opens the vent. Free cooling!
    Otherwise the fan kicks in through the L293D, faster as it gets hotter
  - Every 2 seconds it sends a JSON line over USB so the laptop dashboard can
    plot everything live
  - It also listens for one-letter commands from the dashboard (see handleCommand)

  Libraries you'll need (Arduino IDE, Tools, Manage Libraries):
  - "DHT sensor library" by Adafruit, plus "Adafruit Unified Sensor"
  - "DallasTemperature" by Miles Burton, plus "OneWire"
  - LiquidCrystal and Servo already come with the IDE
*/

#include <DHT.h>
#include <LiquidCrystal.h>
#include <Servo.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <math.h>

// Pins: where everything plugs into the Arduino
const uint8_t PIN_DHT     = 2;
const uint8_t PIN_RGB_R   = 3;   // needs a PWM pin so we can mix colors
const uint8_t PIN_BUZZER  = 4;   // the ACTIVE buzzer, the one with the sealed bottom
const uint8_t PIN_RGB_G   = 5;   // PWM too
const uint8_t PIN_RGB_B   = 6;   // and PWM again
const uint8_t PIN_LCD_RS  = 7;
const uint8_t PIN_LCD_E   = 8;
const uint8_t PIN_SERVO   = 9;
const uint8_t PIN_FAN_IN1 = 10;  // L293D pin 2 (pin 7 on the chip just goes to ground)
const uint8_t PIN_FAN_EN  = 11;  // L293D pin 1, this one controls fan speed
const uint8_t PIN_LCD_D4  = 12;
const uint8_t PIN_LCD_D5  = 13;
const uint8_t PIN_OUTDOOR = A0;  // DS18B20 signal (or the thermistor, if we fall back to it)
const uint8_t PIN_LCD_D6  = A1;
const uint8_t PIN_LCD_D7  = A2;
const uint8_t PIN_BUTTON  = A3;  // other leg goes to ground, the built-in pull-up does the rest
const uint8_t PIN_PIR     = A4;  // HC-SR501 output, for when we add motion sensing

// Settings: tweak these to match your build
const bool USE_DS18B20 = true;    // switch to false to use the kit thermistor instead
const bool USE_PIR     = false;   // flip to true once a PIR is wired to A4

// How long without motion before we call the room empty
const unsigned long EMPTY_AFTER_DEMO_MS = 30000;    // 30 seconds, so judges don't have to wait
const unsigned long EMPTY_AFTER_REAL_MS = 600000;   // 10 minutes for real life

// Heat index thresholds in Fahrenheit for Caution, Warning and Danger.
// The real ones come straight from the National Weather Service heat chart.
// The demo ones are lower so a warm hand or a breath can trigger them at the table!
const float REAL_LEVELS[3] = {80.0, 90.0, 103.0};
const float DEMO_LEVELS[3] = {78.0, 84.0, 90.0};
bool demoMode = true;             // hold the button (or send 'd') to switch between them

const float LEVEL_HYST     = 1.0; // a little buffer so the level doesn't flicker at a threshold
const float OUTDOOR_MARGIN = 3.0; // outside has to be at least this much cooler to open the vent

const int VENT_CLOSED_DEG = 0;
const int VENT_OPEN_DEG   = 90;
const int FAN_MIN_PWM     = 150;  // any lower and our little motor tends to stall
const int FAN_MAX_PWM     = 255;

const unsigned long READ_INTERVAL_MS = 2000;   // the DHT11 can't read faster than about once a second
const unsigned long SCREEN_MS        = 3000;   // how long each LCD page stays up
const unsigned long MUTE_MS          = 300000; // a button press mutes the alarm for 5 minutes
const unsigned long LONG_PRESS_MS    = 1500;

// Only used if we go back to the kit thermistor: a 10k NTC in a voltage divider
// (5V, then a 10k resistor, then A0, then the thermistor, then ground)
const float SERIES_R  = 10000.0;
const float NOMINAL_R = 10000.0;
const float NOMINAL_C = 25.0;
const float BETA      = 3950.0;

// The hardware objects
DHT dht(PIN_DHT, DHT11);
LiquidCrystal lcd(PIN_LCD_RS, PIN_LCD_E, PIN_LCD_D4, PIN_LCD_D5, PIN_LCD_D6, PIN_LCD_D7);
Servo vent;
OneWire oneWire(PIN_OUTDOOR);
DallasTemperature ds18b20(&oneWire);

// Everything the station remembers between loops
enum Mode { MODE_AUTO, MODE_MANUAL };
Mode mode = MODE_AUTO;

float inF = NAN, humidity = NAN, heatIndexF = NAN, outF = NAN;
bool dhtOk = false, outOk = false;
bool occupied = true;
unsigned long lastMotion = 0;
uint8_t level = 0;               // 0 is OK, 1 Caution, 2 Warning, 3 Danger
bool ventOpen = false;
int fanPwm = 0;
unsigned long muteUntil = 0;
bool servoAttached = false;
unsigned long servoMovedAt = 0;

unsigned long lastRead = 0, lastScreen = 0;
uint8_t screen = 0;

// For reading the button cleanly (buttons bounce a lot!)
bool btnStable = HIGH, btnLastRaw = HIGH;
unsigned long btnChangedAt = 0, btnPressedAt = 0;
bool longPressFired = false;

const char* LEVEL_NAMES[4] = {"OK", "CAUTION", "WARNING", "DANGER"};

// Little helpers

bool isMuted() {
  return (long)(muteUntil - millis()) > 0;
}

float readThermistorF(bool &ok) {
  long sum = 0;
  for (uint8_t i = 0; i < 8; i++) sum += analogRead(PIN_OUTDOOR);
  float adc = sum / 8.0;
  ok = (adc > 10 && adc < 1013);          // readings stuck at the edges mean it's unplugged or shorted
  if (!ok) return NAN;
  float r = SERIES_R * adc / (1023.0 - adc);
  float kelvin = 1.0 / (1.0 / (NOMINAL_C + 273.15) + log(r / NOMINAL_R) / BETA);
  return (kelvin - 273.15) * 9.0 / 5.0 + 32.0;
}

float readDs18b20F(bool &ok) {
  float c = ds18b20.getTempCByIndex(0);   // grab the reading we asked for last time
  ds18b20.requestTemperatures();           // and ask for the next one (takes about 750 ms)
  ok = (c != DEVICE_DISCONNECTED_C && c > -50.0 && c < 85.0);
  return ok ? c * 9.0 / 5.0 + 32.0 : NAN;
}

float readOutdoorF(bool &ok) {
  return USE_DS18B20 ? readDs18b20F(ok) : readThermistorF(ok);
}

void updateOccupancy() {
  if (!USE_PIR) { occupied = true; return; }   // no PIR? assume someone's always home
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
    if (i < current) th -= LEVEL_HYST;    // to drop a level, it has to cool a bit past the line
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
  if (pwm > 0 && fanPwm == 0) {           // give the motor a quick full-power kick so it starts spinning
    analogWrite(PIN_FAN_EN, 255);
    delay(150);
  }
  fanPwm = pwm;
  analogWrite(PIN_FAN_EN, fanPwm);
}

void setRgb(uint8_t r, uint8_t g, uint8_t b) {   // our RGB LED is common cathode
  analogWrite(PIN_RGB_R, r);
  analogWrite(PIN_RGB_G, g);
  analogWrite(PIN_RGB_B, b);
}

// The brain: decide how to cool the room

void decideCooling() {
  if (mode != MODE_AUTO || !dhtOk) return;

  if (level == 0 || !occupied) {          // room is comfortable, or nobody's there to cool
    if (ventOpen) setVent(false, false);
    if (fanPwm) setFan(0);
    return;
  }

  // Once the vent is open, keep it open a little longer so it doesn't flap back and forth
  float margin = ventOpen ? OUTDOOR_MARGIN - 1.0 : OUTDOOR_MARGIN;
  bool outsideCooler = outOk && (outF < inF - margin);

  if (outsideCooler) {
    // Cooler outside? Open the vent and save the energy!
    if (!ventOpen) setVent(true, false);
    if (fanPwm) setFan(0);
  } else {
    // Not cooler outside, so the fan takes over. Hotter room, faster fan.
    if (ventOpen) setVent(false, false);
    const float* L = demoMode ? DEMO_LEVELS : REAL_LEVELS;
    float t = (heatIndexF - L[0]) / (L[2] - L[0]);
    t = constrain(t, 0.0, 1.0);
    setFan(FAN_MIN_PWM + (int)(t * (FAN_MAX_PWM - FAN_MIN_PWM)));
  }
}

void updateOutputs() {
  unsigned long now = millis();

  // The LED tells you how it feels at a glance
  if (!dhtOk) {
    setRgb((now / 500) % 2 ? 80 : 0, 0, (now / 500) % 2 ? 80 : 0); // blinking purple means the sensor needs help
  } else if (!occupied) {
    setRgb(0, 0, 40);                                            // dim blue means the room is empty
  } else {
    switch (level) {
      case 0: setRgb(0, 120, 0);   break;                        // green, all good
      case 1: setRgb(255, 150, 0); break;                        // yellow, getting warm
      case 2: setRgb(255, 50, 0);  break;                        // orange, drink some water
      case 3: setRgb((now / 250) % 2 ? 255 : 0, 0, 0); break;    // flashing red, get somewhere cool!
    }
  }

  // The buzzer: steady beeping at Danger, a quick chirp every 30 seconds at Warning
  bool buzz = false;
  if (dhtOk && occupied && !isMuted()) {
    if (level == 3) buzz = (now % 800) < 150;
    else if (level == 2) buzz = (now % 30000) < 100;
  }
  digitalWrite(PIN_BUZZER, buzz ? HIGH : LOW);

  // Let the servo relax once it gets there: no jitter, and it saves battery
  if (servoAttached && now - servoMovedAt > 700) {
    vent.detach();
    servoAttached = false;
  }
}

// The LCD screen

void lcdLine(uint8_t row, const char* text) {
  char buf[17];
  snprintf(buf, sizeof(buf), "%-16s", text);   // pad with spaces so old text gets cleared
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

  // Two pages normally, plus a "drink water" page when it's getting hot
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

// Talking to the laptop over USB

void printNum(const char* key, float v, uint8_t digits, bool valid) {
  Serial.print('"'); Serial.print(key); Serial.print("\":");
  if (valid && !isnan(v)) Serial.print(v, digits); else Serial.print("null");
}

void sendStatus() {
  // One line of JSON with everything the dashboard needs
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

// Commands from the dashboard (or type them in Serial Monitor yourself!):
// a for automatic, f to run the fan, v to open the vent, x to turn everything off,
// m to mute or unmute, d to switch between demo and real thresholds
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
  sendStatus();   // tell the dashboard right away so it feels snappy
}

// The button: short press mutes, long press switches thresholds

void readButton() {
  bool raw = digitalRead(PIN_BUTTON);
  unsigned long now = millis();
  if (raw != btnLastRaw) { btnChangedAt = now; btnLastRaw = raw; }
  if (now - btnChangedAt < 30) return;              // wait for the bouncing to settle

  if (raw != btnStable) {
    btnStable = raw;
    if (btnStable == LOW) {                         // just got pressed
      btnPressedAt = now;
      longPressFired = false;
    } else if (!longPressFired) {                   // let go after a quick tap
      handleCommand('m');
    }
  }
  if (btnStable == LOW && !longPressFired && now - btnPressedAt > LONG_PRESS_MS) {
    longPressFired = true;
    handleCommand('d');
    lcdLine(0, demoMode ? "Demo thresholds" : "Real thresholds");
    lcdLine(1, demoMode ? "78/84/90F" : "80/90/103F");
    lastScreen = now;                               // leave this message up for a moment
  }
}

// Setup runs once when the board powers on

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

  digitalWrite(PIN_FAN_IN1, HIGH);   // the fan only spins one way, speed comes from pin 11
  analogWrite(PIN_FAN_EN, 0);

  dht.begin();
  if (USE_DS18B20) {
    ds18b20.begin();
    ds18b20.setWaitForConversion(false);   // don't freeze everything while it measures
    ds18b20.requestTemperatures();
  }
  lcd.begin(16, 2);
  lcdLine(0, "Heat Station");
  lcdLine(1, "Starting up...");

  setVent(false, true);              // make sure the vent starts closed
  setRgb(0, 0, 120);
  delay(2000);                       // give the DHT11 a moment to wake up
  lastMotion = millis();             // start out assuming someone's in the room
}

// The loop runs forever, over and over

void loop() {
  unsigned long now = millis();

  while (Serial.available()) handleCommand((char)Serial.read());
  readButton();
  updateOccupancy();

  // Every 2 seconds: read the sensors, decide what to do, report to the laptop
  if (now - lastRead >= READ_INTERVAL_MS) {
    lastRead = now;

    float t = dht.readTemperature(true);   // true means Fahrenheit
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

  // Flip to the next LCD page every few seconds
  if (now - lastScreen >= SCREEN_MS) {
    lastScreen = now;
    screen++;
    updateLcd();
  }

  updateOutputs();
}
