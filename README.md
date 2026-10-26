# Heat Safety + Smart Cooling Station (HackIoT 2026)

An Arduino station that measures how hot a room *feels* (heat index), warns people before heat gets dangerous, and cools the room the greenest way available: it opens a vent when outside is cooler and only runs the fan when it isn't. A laptop bridge puts it online with a live dashboard, phone alerts, and a Claude-written room summary.

```
heat-station/
  heat_station/heat_station.ino   Arduino sketch (upload to the UNO)
  bridge/bridge.py                Serial -> dashboard, alerts, Claude summary
  bridge/dashboard.html           Live web dashboard
  bridge/requirements.txt
```

## Wiring

Two power rails: the **UNO 5V** powers the small parts, and the **power supply module** (on the breadboard, fed by the 9V battery) powers the motor and servo. **Connect all grounds together.** Motors on the UNO's 5V pin cause random resets.

| Part | Part pin | Connects to |
|---|---|---|
| DHT11 module | S / middle / - | D2 / UNO 5V / GND |
| RGB LED (common cathode) | longest leg | GND |
| | red, green, blue legs | D3, D5, D6, each through a 220 Ω resistor |
| Active buzzer | + (longer leg) / - | D4 / GND |
| LCD1602 | 1 VSS, 5 RW, 16 K | GND |
| | 2 VDD | UNO 5V |
| | 3 V0 | middle pin of the 10k potentiometer (outer pins to 5V and GND) |
| | 4 RS, 6 E | D7, D8 |
| | 11 D4, 12 D5, 13 D6, 14 D7 | D12, D13, A1, A2 |
| | 15 A (backlight) | 5V through a 220 Ω resistor |
| SG90 servo | brown / red / orange | GND / module 5V / D9 |
| L293D chip | 1 (EN1) | D11 |
| | 2 (IN1) | D10 |
| | 3 (OUT1), 6 (OUT2) | the two fan motor wires |
| | 4, 5, 12, 13 | GND |
| | 7 (IN2) | GND |
| | 8 (VCC2, motor power) | module 5V |
| | 16 (VCC1, logic) | UNO 5V |
| 18B20 temp module (outside) | S / middle / - | A0 / UNO 5V / GND |
| HC-SR501 PIR | OUT / VCC / GND | A4 / UNO 5V / GND (labels are under the white dome) |
| *Fallback only:* thermistor + 10k | divider | 5V, 10k resistor, A0, thermistor, GND. Set `USE_DS18B20 = false` |
| Push button | two diagonal legs | A3 and GND |

L293D pin 1 is next to the notch/dot on the chip. Turn the potentiometer until the LCD text shows up; a blank or solid-block screen is almost always contrast.

## Setup

1. **Arduino IDE:** install "DHT sensor library" by Adafruit (accept "Adafruit Unified Sensor") and "DallasTemperature" by Miles Burton (accept "OneWire"). Set `USE_PIR = true` at the top of the sketch once the PIR is wired. Board: Arduino Uno. Open `heat_station/heat_station.ino` and upload.
2. **If no port shows up:** some Elegoo boards use a CH340 USB chip that needs a driver on Mac/Windows. Install it, replug.
3. **Check:** Tools > Serial Monitor at 9600 baud should print one JSON line every 2 seconds. Close the Serial Monitor before starting the bridge (only one program can use the port).
4. **Bridge:**
   ```
   cd bridge
   pip install -r requirements.txt
   python bridge.py            # auto-detects the Arduino
   python bridge.py --fake     # no hardware: simulator for dashboard work
   ```
   Open http://localhost:8000. Phones on the same Wi-Fi can open http://<laptop-ip>:8000.
5. **Phone alerts (optional):** install the ntfy app, subscribe to a unique topic like `hackiot-heat-yourname`, then run `python bridge.py --ntfy-topic hackiot-heat-yourname`.
6. **Claude summary (optional):** `export ANTHROPIC_API_KEY=...` (Windows: `set ANTHROPIC_API_KEY=...`) before starting the bridge. Change the model with `CLAUDE_MODEL`.

## Controls

| Input | Action |
|---|---|
| Button short press | Mute the alarm for 5 min (press again to unmute) |
| Button long press (1.5 s) | Switch demo thresholds (78/84/90 F) and real NWS thresholds (80/90/103 F) |
| Serial `a f v x m d` | Auto, run fan, open vent, all off, mute, demo toggle (dashboard buttons send these) |

## Demo script (about 2 minutes)

1. Room is normal: green LED, "Comfortable" on the dashboard.
2. Cup your hands around the DHT11 and breathe on it. Humidity and heat index climb, LED goes yellow then orange, LCD says DRINK WATER.
3. Thermistor at room temperature: the fan spins up, faster as it gets hotter.
4. Pinch an ice pack or cold can against the 18B20 ("outside cooled down"): fan stops, servo opens the vent. Point at the energy-saved counter.
5. Keep heating the DHT11 to Danger: red flashing LED, buzzer, phone alert.
6. Everyone steps away from the PIR (or cover it with a cup). After 30 s the LED goes dim blue, cooling and alarm turn off, and "cooling paused in an empty room" starts counting energy saved. Wave at it and everything comes back.
7. Hit "Write summary" and read Claude's summary out loud.

## Honest limits (judges will ask)

- DHT11 is roughly ±2 °C and ±5 % humidity. Fine for a prototype; a real build would use a DHT22 or SHT31.
- kWh saved is an estimate: (vent minutes + empty-room minutes) × an assumed AC power (`--ac-kw`, default 1 kW).
- PIR only sees motion, so someone sitting very still may count as "empty." The 10-minute timeout in real mode covers most of that.
- Privacy: the station stores only temperature, humidity, and a yes/no motion flag, no cameras, microphones, or identities.
