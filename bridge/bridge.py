#!/usr/bin/env python3
"""
HackIoT 2026 Heat Station bridge
Arduino (USB serial) -> live web dashboard + phone alerts (ntfy) + Claude summary

Run with hardware:   python bridge.py
Pick a port:         python bridge.py --port /dev/cu.usbmodem1101   (Windows: --port COM3)
No hardware yet:     python bridge.py --fake
Then open http://localhost:8000
"""
import argparse
import json
import math
import os
import random
import threading
import time
from collections import deque
from pathlib import Path

import requests
from flask import Flask, jsonify, request, send_from_directory

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    serial = None

HERE = Path(__file__).parent
CLAUDE_MODEL = os.environ.get("CLAUDE_MODEL", "claude-haiku-5-5")
ALLOWED_COMMANDS = set("afvxmd")
LEVEL_NAMES = ["OK", "Caution", "Warning", "Danger"]

app = Flask(__name__)
lock = threading.Lock()
state = {
    "latest": None,
    "connected": False,
    "source": "",
    "started": time.time(),
    "vent_seconds": 0.0,
    "fan_seconds": 0.0,
    "danger_seconds": 0.0,
    "empty_seconds": 0.0,   # cooling was needed but the room was empty, so it stayed off
    "last_ts": None,
}
history = deque(maxlen=1800)  # about 1 hour at one reading every 2 s
args = None
link = None  # serial.Serial or FakeArduino


# ---------------------------------------------------------------- data intake
def record(reading: dict):
    """Store one reading from the Arduino and update running totals."""
    now = time.time()
    reading["ts"] = now
    with lock:
        prev_lvl = state["latest"]["lvl"] if state["latest"] else 0
        if state["last_ts"] is not None:
            dt = min(now - state["last_ts"], 10)  # ignore long gaps (unplugged etc.)
            if reading.get("vent"):
                state["vent_seconds"] += dt
            if reading.get("fan"):
                state["fan_seconds"] += dt
            if reading.get("lvl") == 3 and reading.get("occ", 1):
                state["danger_seconds"] += dt
            if reading.get("lvl", 0) >= 1 and not reading.get("occ", 1):
                state["empty_seconds"] += dt
        state["last_ts"] = now
        state["latest"] = reading
        history.append(reading)
    if reading.get("lvl") == 3 and prev_lvl < 3 and reading.get("occ", 1):
        send_phone_alert(reading)


_last_alert = 0.0


def send_phone_alert(reading: dict):
    """Push notification through ntfy.sh (free, no account). Rate limited to one per 10 min."""
    global _last_alert
    topic = args.ntfy_topic or os.environ.get("NTFY_TOPIC")
    if not topic or time.time() - _last_alert < 600:
        return
    _last_alert = time.time()
    msg = (f"Dangerous heat: feels like {reading.get('hi', 0):.0f}F. "
           "Drink water and move somewhere cool.")
    try:
        requests.post(f"https://ntfy.sh/{topic}", data=msg.encode(),
                      headers={"Title": "Heat Station alert", "Priority": "high"}, timeout=5)
        print("[alert] sent:", msg)
    except requests.RequestException as e:
        print("[alert] failed:", e)


def find_port():
    if serial is None:
        raise SystemExit("pyserial is missing. Run: pip install -r requirements.txt")
    hints = ("arduino", "usbmodem", "usbserial", "wchusb", "ch340", "ttyacm", "ttyusb")
    for p in serial.tools.list_ports.comports():
        text = f"{p.device} {p.description} {p.manufacturer}".lower()
        if any(h in text for h in hints):
            return p.device
    ports = [p.device for p in serial.tools.list_ports.comports()]
    raise SystemExit(f"No Arduino found. Ports seen: {ports or 'none'}. Use --port.")


def serial_loop():
    global link
    while True:
        try:
            port = args.port or find_port()
            link = serial.Serial(port, 9600, timeout=2)
            with lock:
                state["connected"], state["source"] = True, port
            print(f"[serial] connected on {port}")
            while True:
                line = link.readline().decode(errors="ignore").strip()
                if not line.startswith("{"):
                    continue  # skip boot noise or partial lines
                try:
                    record(json.loads(line))
                except json.JSONDecodeError:
                    pass
        except SystemExit as e:
            print(f"[serial] {e}")
            time.sleep(3)
        except Exception as e:  # unplugged, permission error, etc.
            print(f"[serial] {e}. Retrying in 2 s")
            with lock:
                state["connected"] = False
            time.sleep(2)


# ---------------------------------------------------------------- fake Arduino
class FakeArduino:
    """Simulates the station so the dashboard can be built without hardware."""

    def __init__(self):
        self.mode, self.mute, self.demo = "auto", False, True
        self.manual = {"fan": 0, "vent": 0}
        self.last_motion = time.time()

    def write(self, data: bytes):
        c = data.decode()[:1]
        if c == "a": self.mode = "auto"
        if c == "f": self.mode, self.manual = "manual", {"fan": 100, "vent": 0}
        if c == "v": self.mode, self.manual = "manual", {"fan": 0, "vent": 1}
        if c == "x": self.mode, self.manual = "manual", {"fan": 0, "vent": 0}
        if c == "m": self.mute = not self.mute
        if c == "d": self.demo = not self.demo

    def loop(self):
        with lock:
            state["connected"], state["source"] = True, "simulator"
        t0 = time.time()
        while True:
            t = time.time() - t0
            in_f = 79 + 9 * math.sin(t / 40) + random.uniform(-0.3, 0.3)
            hum = 55 + 15 * math.sin(t / 55 + 1) + random.uniform(-1, 1)
            out_f = 76 + 6 * math.sin(t / 90 + 2)
            hi = heat_index(in_f, hum)
            levels = (78, 84, 90) if self.demo else (80, 90, 103)
            lvl = sum(hi >= x for x in levels)
            if (t // 45) % 3 != 2:            # people around 2 out of every 3 periods
                self.last_motion = time.time()
            occ = int(time.time() - self.last_motion < 30)
            if self.mode == "auto" and not occ:
                vent, fan = 0, 0
            elif self.mode == "auto":
                vent = int(lvl > 0 and out_f < in_f - 3)
                fan = 0 if (lvl == 0 or vent) else int(59 + 41 * min(max((hi - levels[0]) / (levels[2] - levels[0]), 0), 1))
            else:
                vent, fan = self.manual["vent"], self.manual["fan"]
            record({"in": round(in_f, 1), "hum": round(hum), "hi": round(hi, 1),
                    "out": round(out_f, 1), "lvl": lvl, "fan": fan, "vent": vent,
                    "occ": occ, "pir": 1, "mute": int(self.mute), "mode": self.mode, "demo": int(self.demo), "err": 0})
            time.sleep(2)


def heat_index(t, rh):
    """NWS heat index (same math as the Adafruit DHT library)."""
    hi = 0.5 * (t + 61.0 + (t - 68.0) * 1.2 + rh * 0.094)
    if hi > 79:
        hi = (-42.379 + 2.04901523 * t + 10.14333127 * rh - 0.22475541 * t * rh
              - 0.00683783 * t * t - 0.05481717 * rh * rh + 0.00122874 * t * t * rh
              + 0.00085282 * t * rh * rh - 0.00000199 * t * t * rh * rh)
        if rh < 13 and 80 <= t <= 112:
            hi -= ((13 - rh) * 0.25) * math.sqrt((17 - abs(t - 95)) * 0.05882)
        elif rh > 85 and 80 <= t <= 87:
            hi += ((rh - 85) * 0.1) * ((87 - t) * 0.2)
    return hi


# ---------------------------------------------------------------- web
def stats_snapshot():
    kwh = (state["vent_seconds"] + state["empty_seconds"]) / 3600 * args.ac_kw
    return {
        "vent_minutes": round(state["vent_seconds"] / 60, 1),
        "fan_minutes": round(state["fan_seconds"] / 60, 1),
        "danger_minutes": round(state["danger_seconds"] / 60, 1),
        "empty_minutes": round(state["empty_seconds"] / 60, 1),
        "kwh_avoided": round(kwh, 3),
        "co2_lbs_avoided": round(kwh * args.co2_lbs_per_kwh, 2),
        "uptime_minutes": round((time.time() - state["started"]) / 60, 1),
        "ac_kw": args.ac_kw,
    }


@app.get("/")
def index():
    return send_from_directory(HERE, "dashboard.html")


@app.get("/api/state")
def api_state():
    with lock:
        cutoff = time.time() - 15 * 60
        recent = [h for h in history if h["ts"] >= cutoff]
        return jsonify({"latest": state["latest"], "connected": state["connected"],
                        "source": state["source"], "stats": stats_snapshot(),
                        "history": recent})


@app.post("/api/cmd")
def api_cmd():
    c = (request.get_json(silent=True) or {}).get("c", "")
    if c not in ALLOWED_COMMANDS or len(c) != 1:
        return jsonify({"ok": False, "error": "Unknown command"}), 400
    if link is None:
        return jsonify({"ok": False, "error": "Station is not connected"}), 503
    link.write(c.encode())
    return jsonify({"ok": True})


@app.post("/api/summary")
def api_summary():
    key = os.environ.get("ANTHROPIC_API_KEY")
    if not key:
        return jsonify({"ok": False, "text": "Set ANTHROPIC_API_KEY before starting the bridge to get summaries."})
    with lock:
        recent = list(history)[-450:]  # last 15 min
        stats = stats_snapshot()
    if not recent:
        return jsonify({"ok": False, "text": "No readings yet. Check that the station is connected."})
    sample = [{k: r.get(k) for k in ("in", "hum", "hi", "out", "lvl", "fan", "vent", "occ")} for r in recent[::15]]
    prompt = (
        "You are the voice of a heat-safety station in a USC room built for a sustainability hackathon. "
        "Readings are in Fahrenheit; 'hi' is the heat index, 'lvl' is 0 OK, 1 Caution, 2 Warning, 3 Danger; "
        "'vent' means the station opened a vent instead of running cooling; 'fan' is fan speed in percent; "
        "'occ' is 1 when a motion sensor sees people, 0 when the room is empty and cooling is paused.\n"
        f"Totals so far: {json.dumps(stats)}\n"
        f"Readings from the last 15 minutes (every 30 s): {json.dumps(sample)}\n\n"
        "Write 3 short sentences for the people in the room: the current heat risk and what to do, "
        "how much cooling energy the vent and the empty-room pause saved, and one practical tip. Plain language, no lists, no em dashes."
    )
    try:
        r = requests.post(
            "https://api.anthropic.com/v1/messages",
            headers={"x-api-key": key, "anthropic-version": "2023-06-01", "content-type": "application/json"},
            json={"model": CLAUDE_MODEL, "max_tokens": 300, "messages": [{"role": "user", "content": prompt}]},
            timeout=30,
        )
        r.raise_for_status()
        text = "".join(b.get("text", "") for b in r.json().get("content", []) if b.get("type") == "text")
        return jsonify({"ok": True, "text": text.strip()})
    except requests.RequestException as e:
        return jsonify({"ok": False, "text": f"Summary failed: {e}"}), 502


def main():
    global args, link
    p = argparse.ArgumentParser(description="Heat Station bridge")
    p.add_argument("--port", help="Serial port of the Arduino (auto-detected if omitted)")
    p.add_argument("--fake", action="store_true", help="Simulate the station, no hardware needed")
    p.add_argument("--ntfy-topic", help="ntfy.sh topic for phone alerts (or env NTFY_TOPIC)")
    p.add_argument("--ac-kw", type=float, default=1.0, help="Power of the AC the vent replaces, in kW")
    p.add_argument("--co2-lbs-per-kwh", type=float, default=0.5, help="Grid emissions factor")
    p.add_argument("--host", default="0.0.0.0", help="0.0.0.0 lets phones on the same Wi-Fi open it")
    p.add_argument("--web-port", type=int, default=8000)
    args = p.parse_args()

    if args.fake:
        link = FakeArduino()
        threading.Thread(target=link.loop, daemon=True).start()
    else:
        threading.Thread(target=serial_loop, daemon=True).start()

    print(f"Dashboard: http://localhost:{args.web_port}")
    app.run(host=args.host, port=args.web_port, debug=False, threaded=True)


if __name__ == "__main__":
    main()
