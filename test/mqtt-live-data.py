#!/usr/bin/env python3
"""
MQTT Test Data Publisher for Home Assistant
============================================
Publishes simulated sensor data at 1 Hz to test real-time plotting
on the ESP32-S3 AMOLED panel.

Sensors created:
  sensor.test_sine_wave     → test/sensors/sine_wave       (°C, 18-26°C sine wave)
  sensor.test_noisy_signal  → test/sensors/noisy_signal    (hPa, 1010-1020 with noise)

Usage:
  pip install paho-mqtt
  python mqtt_test_publisher.py

  # Broker credentials are read from secrets.json (gitignored) next to this
  # script. Copy secrets.example.json → secrets.json and fill it in.

  # Override broker settings on the CLI (takes precedence over secrets.json):
  python mqtt_test_publisher.py --host 192.168.1.100 --port 1883 \
      --username mqtt_user1 --password ********

  # Different rate or waveforms:
  python mqtt_test_publisher.py --hz 2 --waveform1 sawtooth --waveform2 chirp

  # Explicit IP if mDNS doesn't resolve:
  python mqtt_test_publisher.py --host 192.168.1.xxx
"""

import argparse
import json
import math
import os
import random
import signal
import sys
import time

try:
    import paho.mqtt.client as mqtt
except ImportError:
    print("ERROR: paho-mqtt not installed. Run: pip install paho-mqtt")
    sys.exit(1)

# ── Broker settings — loaded from gitignored secrets.json ─────────────────────
# Copy secrets.example.json → secrets.json and fill in your broker credentials.
SECRETS_FILE = os.path.join(os.path.dirname(__file__), "secrets.json")


def load_secrets():
    """Read broker credentials from secrets.json (gitignored). Missing file
    is fine — defaults / CLI args cover it."""
    try:
        with open(SECRETS_FILE, encoding="utf-8") as f:
            return json.load(f)
    except FileNotFoundError:
        return {}
    except (json.JSONDecodeError, OSError) as e:
        print(f"WARNING: could not read {SECRETS_FILE}: {e}")
        return {}


_secrets = load_secrets()

DEFAULT_HOST     = _secrets.get("host", "homeassistant.local")   # or your HA IP
DEFAULT_PORT     = int(_secrets.get("port", 1883))
DEFAULT_USERNAME = _secrets.get("username", "mqtt_user1")
DEFAULT_PASSWORD = _secrets.get("password", "")

# ── Topic map ─────────────────────────────────────────────────────────────────
TOPICS = {
    "sine_wave":    "test/sensors/sine_wave",
    "noisy_signal": "test/sensors/noisy_signal",
}

# ── Waveform generators ───────────────────────────────────────────────────────

def sine_wave(t, period=60.0, min_val=18.0, max_val=26.0):
    """Smooth sine wave — good for testing smooth plot rendering."""
    midpoint = (min_val + max_val) / 2
    amplitude = (max_val - min_val) / 2
    return midpoint + amplitude * math.sin(2 * math.pi * t / period)


def noisy_signal(t, period=120.0, min_val=1010.0, max_val=1020.0, noise_scale=0.4):
    """Sine wave with Gaussian noise — tests plot stability under jitter."""
    base = sine_wave(t, period=period, min_val=min_val, max_val=max_val)
    return base + random.gauss(0, noise_scale)


def sawtooth(t, period=30.0, min_val=0.0, max_val=100.0):
    """Sawtooth wave — tests rapid value jumps."""
    phase = (t % period) / period
    return min_val + (max_val - min_val) * phase


def square_wave(t, period=20.0, low=0.0, high=100.0):
    """Square wave — tests binary-like transitions."""
    return high if (t % period) < (period / 2) else low


def chirp(t, f0=0.01, f1=0.5, chirp_period=120.0, amplitude=50.0, offset=50.0):
    """Frequency sweep (chirp) — tests rendering at varying update speeds."""
    freq = f0 + (f1 - f0) * (t % chirp_period) / chirp_period
    return offset + amplitude * math.sin(2 * math.pi * freq * t)


# ── Waveform registry (select with --waveform1 / --waveform2) ─────────────────
WAVEFORMS = {
    "sine":     lambda t: sine_wave(t),
    "noisy":    lambda t: noisy_signal(t),
    "sawtooth": lambda t: sawtooth(t),
    "square":   lambda t: square_wave(t),
    "chirp":    lambda t: chirp(t),
}

# ── MQTT client setup ─────────────────────────────────────────────────────────

def build_client(args):
    client = mqtt.Client(client_id="ha_test_publisher", protocol=mqtt.MQTTv311)
    if args.username:
        client.username_pw_set(args.username, args.password)

    def on_connect(client, userdata, flags, rc):
        codes = {
            0: "Connected",
            1: "Bad protocol version",
            2: "Client ID rejected",
            3: "Broker unavailable",
            4: "Bad credentials",
            5: "Not authorised",
        }
        if rc == 0:
            print(f"✓ {codes[rc]} to {args.host}:{args.port}")
        else:
            print(f"✗ Connection failed: {codes.get(rc, f'rc={rc}')}")
            sys.exit(1)

    def on_disconnect(client, userdata, rc):
        if rc != 0:
            print(f"⚠ Unexpected disconnect (rc={rc}), reconnecting…")

    client.on_connect = on_connect
    client.on_disconnect = on_disconnect
    return client


# ── Main publish loop ─────────────────────────────────────────────────────────

def run(args):
    client = build_client(args)

    print(f"Connecting to {args.host}:{args.port}…")
    client.connect(args.host, args.port, keepalive=60)
    client.loop_start()
    time.sleep(1.0)  # let on_connect fire

    fn1 = WAVEFORMS[args.waveform1]
    fn2 = WAVEFORMS[args.waveform2]

    t = 0.0
    interval = 1.0 / args.hz

    print(f"\nPublishing at {args.hz} Hz  |  Ctrl-C to stop\n")
    print(f"  Sensor 1 ({args.waveform1:>8}): {TOPICS['sine_wave']}")
    print(f"  Sensor 2 ({args.waveform2:>8}): {TOPICS['noisy_signal']}\n")

    running = True

    def stop(sig, frame):
        nonlocal running
        print("\nStopping…")
        running = False

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)

    tick = 0
    while running:
        start = time.monotonic()

        v1 = round(fn1(t), 2)
        v2 = round(fn2(t), 2)

        client.publish(TOPICS["sine_wave"],    str(v1), qos=0)
        client.publish(TOPICS["noisy_signal"], str(v2), qos=0)

        tick += 1
        if tick % 10 == 0:
            print(f"  t={t:7.1f}s  |  sine_wave={v1:7.2f}  noisy_signal={v2:8.2f}")

        t += interval
        elapsed = time.monotonic() - start
        sleep_for = interval - elapsed
        if sleep_for > 0:
            time.sleep(sleep_for)

    client.loop_stop()
    client.disconnect()
    print("Done.")


# ── CLI ───────────────────────────────────────────────────────────────────────

def parse_args():
    p = argparse.ArgumentParser(
        description="Publish simulated sensor data to MQTT at 1 Hz for HA plotting tests."
    )
    p.add_argument("--host",      default=DEFAULT_HOST,     help="MQTT broker host")
    p.add_argument("--port",      default=DEFAULT_PORT,     type=int)
    p.add_argument("--username",  default=DEFAULT_USERNAME)
    p.add_argument("--password",  default=DEFAULT_PASSWORD)
    p.add_argument("--hz",        default=1.0, type=float,  help="Publish rate in Hz (default: 1)")
    p.add_argument("--waveform1", default="sine",   choices=WAVEFORMS.keys(),
                   help="Waveform for sensor 1 (default: sine)")
    p.add_argument("--waveform2", default="noisy",  choices=WAVEFORMS.keys(),
                   help="Waveform for sensor 2 (default: noisy)")
    return p.parse_args()


if __name__ == "__main__":
    run(parse_args())