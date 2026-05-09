"""
Text-to-Speech Virtual Microphone
==================================
Type text, press Enter, and the speech audio is routed to a virtual
audio cable so other apps (Discord, Zoom, etc.) hear it as mic input.

Uses Google Cloud Text-to-Speech API with high-quality Wavenet voices.

Requirements:
  - Python 3.9+
  - VB-Cable (free) or VoiceMeeter installed for the virtual mic device
  - ffmpeg on PATH (needed by pydub to decode MP3)

Usage:
  pip install -r requirements.txt
  python tts_mic.py
"""

import asyncio
import base64
import io
import json
import os
import socket
import sys
import threading
import tkinter as tk
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from tkinter import messagebox, ttk

import numpy as np
import requests
import sounddevice as sd
from pydub import AudioSegment

# ── Default TTS settings ────────────────────────────────────────
APP_VERSION = "2.0.0"

APP_DIR = Path(__file__).resolve().parent
LOCAL_CONFIG_PATH = APP_DIR / "local_config.json"


def load_google_tts_api_key():
    env_value = os.environ.get("GOOGLE_TTS_API_KEY", "").strip()
    if env_value:
        return env_value

    try:
        config = json.loads(LOCAL_CONFIG_PATH.read_text(encoding="utf-8"))
    except FileNotFoundError as exc:
        raise RuntimeError(
            "Missing pc_app/local_config.json. Copy pc_app/local_config.example.json "
            "and add your Google Cloud API key, or set GOOGLE_TTS_API_KEY."
        ) from exc

    api_key = str(config.get("google_tts_api_key", "")).strip()
    if not api_key:
        raise RuntimeError(
            "pc_app/local_config.json does not contain google_tts_api_key."
        )
    return api_key


GOOGLE_TTS_API_KEY = load_google_tts_api_key()
GOOGLE_TTS_URL = "https://texttospeech.googleapis.com/v1/text:synthesize"

DEFAULT_VOICE = "en-US-Neural2-C"
DEFAULT_RATE = 1.0        # 0.25 to 4.0 (1.0 = normal speed)
DEFAULT_PITCH = 0.0       # -20.0 to 20.0 semitones

# Available voices: Google Cloud Wavenet/Neural2 voices (premium quality, matches Google devices)
VOICES = {
    "1": ("en-US-Neural2-A", "Aria (US Female) - Neural2"),
    "2": ("en-US-Neural2-C", "Christopher (US Male) - Neural2"),
    "3": ("en-US-Neural2-E", "Joanna (US Female) - Neural2"),
    "4": ("en-US-Neural2-F", "Joseph (US Male) - Neural2"),
    "5": ("en-GB-Neural2-A", "Amy (UK Female) - Neural2"),
    "6": ("en-GB-Neural2-B", "Brian (UK Male) - Neural2"),
    "7": ("en-GB-Neural2-C", "Diana (UK Female) - Neural2"),
    "8": ("en-GB-Neural2-D", "Oliver (UK Male) - Neural2"),
}

TEST_MESSAGE = "This is a test message. If you can hear this, output routing is working."


def find_preferred_device(devices):
    if not devices:
        return None, ""

    idx = 0
    for i, (_, name) in enumerate(devices):
        low = name.lower()
        if "cable" in low or "voicemeeter" in low:
            idx = i
            break
    return devices[idx]


def list_output_devices():
    """Return list of (device_index, device_name) for output-capable devices."""
    devices = sd.query_devices()
    output_devs = []
    for i, d in enumerate(devices):
        if d["max_output_channels"] > 0:
            output_devs.append((i, d["name"]))
    return output_devs


def pick_device(devices):
    """Let the user select an audio output device by number."""
    print("\nAvailable audio output devices:")
    for idx, (dev_id, name) in enumerate(devices):
        tag = " ← likely VB-Cable" if "cable" in name.lower() else ""
        print(f"  [{idx}] {name}{tag}")
    print()

    while True:
        choice = input("Select device number for virtual mic output: ").strip()
        try:
            sel = int(choice)
            if 0 <= sel < len(devices):
                return devices[sel][0], devices[sel][1]
        except ValueError:
            pass
        print("  Invalid selection, try again.")


async def synthesize(text, voice, rate, pitch):
    """Call Google Cloud Text-to-Speech API and return raw MP3 bytes."""
    payload = {
        "input": {"text": text},
        "voice": {
            "languageCode": "en-US" if voice.startswith("en-US") else "en-GB",
            "name": voice,
        },
        "audioConfig": {
            "audioEncoding": "MP3",
            "speakingRate": float(rate),
            "pitch": float(pitch),
        },
    }

    try:
        response = requests.post(
            f"{GOOGLE_TTS_URL}?key={GOOGLE_TTS_API_KEY}",
            json=payload,
            timeout=10,
        )
        response.raise_for_status()
        data = response.json()

        if "audioContent" not in data:
            raise ValueError("No audio in response")

        # Decode base64 audio
        audio_bytes = base64.b64decode(data["audioContent"])
        return audio_bytes

    except requests.exceptions.RequestException as e:
        raise RuntimeError(f"Google Cloud TTS API error: {e}")
    except Exception as e:
        raise RuntimeError(f"Synthesis error: {e}")


def play_to_device(mp3_data, device_index):
    """Decode MP3 and play through a specific audio output device."""
    audio = AudioSegment.from_mp3(io.BytesIO(mp3_data))

    # Convert to numpy float32 array normalised to [-1, 1]
    samples = np.array(audio.get_array_of_samples(), dtype=np.float32)
    max_val = float(2 ** (audio.sample_width * 8 - 1))
    samples /= max_val

    if audio.channels == 2:
        samples = samples.reshape(-1, 2)

    sd.play(samples, samplerate=audio.frame_rate, device=device_index)
    sd.wait()


def print_help():
    print("""
Commands (type instead of text):
  /voice       - change voice
  /rate  <val> - set speech rate   (e.g. /rate 1.5  or /rate 0.8)  [0.25 to 4.0]
  /pitch <val> - set pitch offset  (e.g. /pitch 5.0 or /pitch -3.0) [-20 to 20 semitones]
  /device      - change output device
  /settings    - show current settings
  /help        - show this help
  /quit        - exit
""")


def pick_voice():
    """Let user choose a voice from the preset list."""
    print("\nAvailable voices:")
    for key, (_, label) in VOICES.items():
        print(f"  [{key}] {label}")
    print()
    while True:
        choice = input("Select voice number: ").strip()
        if choice in VOICES:
            return VOICES[choice][0], VOICES[choice][1]
        print("  Invalid selection, try again.")


def cli_main():
    print("=" * 50)
    print("  Text-to-Speech  →  Virtual Microphone")
    print("=" * 50)

    # ── Select output device ──
    devices = list_output_devices()
    if not devices:
        print("ERROR: No audio output devices found.")
        sys.exit(1)

    device_index, device_name = pick_device(devices)
    print(f"\n  Output device: {device_name}")

    # ── Current settings ──
    voice = DEFAULT_VOICE
    voice_label = "Christopher (US Male) - Neural2"
    rate = DEFAULT_RATE
    pitch = DEFAULT_PITCH

    print(f"  Voice: {voice_label}")
    print(f"  Rate:  {rate}   Pitch: {pitch}")
    print("\nType text and press Enter to speak.  Type /help for commands.\n")

    while True:
        try:
            text = input("> ").strip()
        except (EOFError, KeyboardInterrupt):
            print("\nBye!")
            break

        if not text:
            continue

        # ── Handle commands ──
        if text.startswith("/"):
            cmd = text.split()
            cmd_name = cmd[0].lower()

            if cmd_name in ("/quit", "/exit", "/q"):
                print("Bye!")
                break

            elif cmd_name == "/help":
                print_help()

            elif cmd_name == "/voice":
                voice, voice_label = pick_voice()
                print(f"  Voice set to: {voice_label}\n")

            elif cmd_name == "/rate":
                if len(cmd) >= 2:
                    try:
                        rate = float(cmd[1])
                        if 0.25 <= rate <= 4.0:
                            print(f"  Rate set to: {rate}\n")
                        else:
                            print(f"  Rate must be between 0.25 and 4.0\n")
                            rate = DEFAULT_RATE
                    except ValueError:
                        print(f"  Invalid rate: {cmd[1]}\n")
                        rate = DEFAULT_RATE
                else:
                    print("  Usage: /rate 1.5\n")

            elif cmd_name == "/pitch":
                if len(cmd) >= 2:
                    try:
                        pitch = float(cmd[1])
                        if -20.0 <= pitch <= 20.0:
                            print(f"  Pitch set to: {pitch}\n")
                        else:
                            print(f"  Pitch must be between -20.0 and 20.0\n")
                            pitch = DEFAULT_PITCH
                    except ValueError:
                        print(f"  Invalid pitch: {cmd[1]}\n")
                        pitch = DEFAULT_PITCH
                else:
                    print("  Usage: /pitch 5.0\n")

            elif cmd_name == "/device":
                devices = list_output_devices()
                device_index, device_name = pick_device(devices)
                print(f"  Output device: {device_name}\n")

            elif cmd_name == "/settings":
                print(f"  Device: {device_name}")
                print(f"  Voice:  {voice_label}")
                print(f"  Rate:   {rate}")
                print(f"  Pitch:  {pitch}\n")

            else:
                print(f"  Unknown command: {cmd_name}  (type /help)\n")

            continue

        # ── Synthesize and play ──
        try:
            print("  Synthesizing...", end="", flush=True)
            mp3_data = asyncio.run(synthesize(text, voice, rate, pitch))
            print(f" ({len(mp3_data) // 1024} KB)  Playing...", end="", flush=True)
            play_to_device(mp3_data, device_index)
            print("  Done.\n")
        except Exception as e:
            print(f"\n  ERROR: {e}\n")


class TTSMicGUI:
    def __init__(self):
        self.root = tk.Tk()
        self.root.title(f"TTS Virtual Mic v{APP_VERSION}")
        self.root.geometry("780x520")

        self.voice = DEFAULT_VOICE
        self.voice_label = "Christopher (US Male) - Neural2"
        self.rate = DEFAULT_RATE
        self.pitch = DEFAULT_PITCH
        self.devices = []
        self.is_busy = False

        self.status_var = tk.StringVar(value="Ready")
        self.voice_var = tk.StringVar(value=self.voice_label)
        self.rate_var = tk.StringVar(value=str(self.rate))
        self.pitch_var = tk.StringVar(value=str(self.pitch))
        self.device_var = tk.StringVar()

        self._build_ui()
        self.refresh_devices()

    def _build_ui(self):
        self.root.columnconfigure(0, weight=1)
        self.root.rowconfigure(1, weight=1)

        top = ttk.Frame(self.root, padding=10)
        top.grid(row=0, column=0, sticky="ew")
        top.columnconfigure(1, weight=1)

        ttk.Label(top, text=f"TTS Virtual Mic v{APP_VERSION}").grid(
            row=0, column=0, columnspan=6, sticky="w", pady=(0, 8)
        )

        ttk.Label(top, text="Voice:").grid(row=1, column=0, sticky="w", padx=(0, 8))
        voice_labels = [label for _, label in VOICES.values()]
        self.voice_combo = ttk.Combobox(
            top,
            textvariable=self.voice_var,
            values=voice_labels,
            state="readonly",
        )
        self.voice_combo.grid(row=1, column=1, sticky="ew", padx=(0, 10))
        self.voice_combo.bind("<<ComboboxSelected>>", self.on_voice_changed)

        ttk.Label(top, text="Rate:").grid(row=1, column=2, sticky="w", padx=(0, 8))
        ttk.Entry(top, textvariable=self.rate_var, width=10).grid(row=1, column=3, sticky="w", padx=(0, 10))
        ttk.Label(top, text="Pitch:").grid(row=1, column=4, sticky="w", padx=(0, 8))
        ttk.Entry(top, textvariable=self.pitch_var, width=10).grid(row=1, column=5, sticky="w")

        ttk.Label(top, text="Output Device:").grid(row=2, column=0, sticky="w", pady=(10, 0), padx=(0, 8))
        self.device_combo = ttk.Combobox(top, textvariable=self.device_var, state="readonly")
        self.device_combo.grid(row=2, column=1, columnspan=4, sticky="ew", pady=(10, 0), padx=(0, 10))
        ttk.Button(top, text="Refresh", command=self.refresh_devices).grid(row=2, column=5, sticky="e", pady=(10, 0))

        center = ttk.Frame(self.root, padding=(10, 0, 10, 10))
        center.grid(row=1, column=0, sticky="nsew")
        center.columnconfigure(0, weight=1)
        center.rowconfigure(0, weight=1)

        self.text_input = tk.Text(center, wrap="word", height=12)
        self.text_input.grid(row=0, column=0, sticky="nsew")
        self.text_input.insert("1.0", "Type text here and click Speak.")

        buttons = ttk.Frame(center)
        buttons.grid(row=1, column=0, sticky="ew", pady=(10, 0))
        buttons.columnconfigure(0, weight=1)
        buttons.columnconfigure(1, weight=1)
        buttons.columnconfigure(2, weight=1)

        ttk.Button(buttons, text="Speak", command=self.on_speak).grid(row=0, column=0, sticky="ew", padx=(0, 6))
        ttk.Button(buttons, text="Test Output", command=self.on_test_output).grid(row=0, column=1, sticky="ew", padx=6)
        ttk.Button(buttons, text="Quit", command=self.root.destroy).grid(row=0, column=2, sticky="ew", padx=(6, 0))

        status = ttk.Label(self.root, textvariable=self.status_var, anchor="w", padding=(10, 8))
        status.grid(row=2, column=0, sticky="ew")

    def set_status(self, message):
        self.status_var.set(message)

    def on_voice_changed(self, _event=None):
        selected = self.voice_var.get()
        for voice_id, label in VOICES.values():
            if label == selected:
                self.voice = voice_id
                self.voice_label = label
                return

    def refresh_devices(self):
        self.devices = list_output_devices()
        if not self.devices:
            self.device_combo["values"] = []
            self.device_var.set("")
            self.set_status("No output devices found.")
            return

        labels = [name for _, name in self.devices]
        self.device_combo["values"] = labels

        preferred_device_id, preferred_device_name = find_preferred_device(self.devices)
        default_idx = 0
        for i, (dev_id, name) in enumerate(self.devices):
            if dev_id == preferred_device_id and name == preferred_device_name:
                default_idx = i
                break

        if self.device_var.get() in labels:
            return

        self.device_combo.current(default_idx)
        self.set_status(f"Selected output device: {labels[default_idx]}")

    def get_selected_device(self):
        selected_name = self.device_var.get()
        for dev_id, name in self.devices:
            if name == selected_name:
                return dev_id, name
        return None, None

    def on_speak(self):
        text = self.text_input.get("1.0", "end").strip()
        if not text:
            messagebox.showwarning("No text", "Please enter text to speak.")
            return
        self.start_synthesis(text)

    def on_test_output(self):
        self.start_synthesis(TEST_MESSAGE)

    def start_synthesis(self, text):
        if self.is_busy:
            self.set_status("Busy: wait for current playback to finish.")
            return

        device_id, device_name = self.get_selected_device()
        if device_id is None:
            messagebox.showerror("No output device", "Please choose an output device first.")
            return

        # Parse rate and pitch as floats
        try:
            rate = float(self.rate_var.get().strip()) if self.rate_var.get().strip() else DEFAULT_RATE
            pitch = float(self.pitch_var.get().strip()) if self.pitch_var.get().strip() else DEFAULT_PITCH
        except ValueError:
            messagebox.showerror("Invalid input", "Rate and Pitch must be numbers.")
            return

        self.rate = rate
        self.pitch = pitch
        self.is_busy = True
        self.set_status(f"Synthesizing with {self.voice_label} -> {device_name}...")

        thread = threading.Thread(
            target=self._synthesize_and_play,
            args=(text, device_id, device_name),
            daemon=True,
        )
        thread.start()

    def _synthesize_and_play(self, text, device_id, device_name):
        try:
            mp3_data = asyncio.run(synthesize(text, self.voice, self.rate, self.pitch))
            self.root.after(0, lambda: self.set_status(f"Playing on {device_name}..."))
            play_to_device(mp3_data, device_id)
            self.root.after(0, lambda: self.set_status("Done."))
        except Exception as exc:
            self.root.after(0, lambda: messagebox.showerror("TTS error", str(exc)))
            self.root.after(0, lambda: self.set_status("Error while synthesizing or playing audio."))
        finally:
            self.root.after(0, self._clear_busy)

    def _clear_busy(self):
        self.is_busy = False

    def run(self):
        self.root.mainloop()


def gui_main():
    app = TTSMicGUI()
    app.run()


class WebTTSService:
        def __init__(self):
                self.lock = threading.Lock()
                self.is_busy = False
                self.last_status = "Ready"
                self.last_error = ""

                self.voice = DEFAULT_VOICE
                self.rate = DEFAULT_RATE
                self.pitch = DEFAULT_PITCH

                self.devices = list_output_devices()
                preferred_id, preferred_name = find_preferred_device(self.devices)
                self.device_id = preferred_id
                self.device_name = preferred_name

        def refresh_devices(self):
                with self.lock:
                        self.devices = list_output_devices()
                        if not self.devices:
                                self.device_id = None
                                self.device_name = ""
                                return

                        names = [name for _, name in self.devices]
                        if self.device_name in names:
                                for dev_id, name in self.devices:
                                        if name == self.device_name:
                                                self.device_id = dev_id
                                                self.device_name = name
                                                return

                        preferred_id, preferred_name = find_preferred_device(self.devices)
                        self.device_id = preferred_id
                        self.device_name = preferred_name

        def set_settings(self, payload):
                with self.lock:
                        voice = payload.get("voice", self.voice)
                        rate = payload.get("rate", self.rate)
                        pitch = payload.get("pitch", self.pitch)
                        device_name = payload.get("device", self.device_name)

                        valid_voice_ids = [voice_id for voice_id, _ in VOICES.values()]
                        if voice in valid_voice_ids:
                                self.voice = voice

                        # Parse rate and pitch as floats
                        try:
                                self.rate = float(rate) if rate else DEFAULT_RATE
                                self.pitch = float(pitch) if pitch else DEFAULT_PITCH
                        except (ValueError, TypeError):
                                self.rate = DEFAULT_RATE
                                self.pitch = DEFAULT_PITCH

                        for dev_id, name in self.devices:
                                if name == device_name:
                                        self.device_id = dev_id
                                        self.device_name = name
                                        break

        def get_state(self):
                with self.lock:
                        voice_options = [{"id": voice_id, "label": label} for voice_id, label in VOICES.values()]
                        devices = [name for _, name in self.devices]
                        return {
                                "voice": self.voice,
                                "rate": self.rate,
                                "pitch": self.pitch,
                                "device": self.device_name,
                                "devices": devices,
                                "voices": voice_options,
                                "busy": self.is_busy,
                                "status": self.last_status,
                                "error": self.last_error,
                        }

        def start_speak(self, text):
                with self.lock:
                        if self.is_busy:
                                return False, "Busy"
                        if not self.device_name:
                                return False, "No output device selected"

                        device_id = self.device_id
                        device_name = self.device_name
                        voice = self.voice
                        rate = self.rate
                        pitch = self.pitch
                        self.is_busy = True
                        self.last_error = ""
                        self.last_status = f"Synthesizing -> {device_name}"

                worker = threading.Thread(
                        target=self._speak_worker,
                        args=(text, device_id, device_name, voice, rate, pitch),
                        daemon=True,
                )
                worker.start()
                return True, "Started"

        def _speak_worker(self, text, device_id, device_name, voice, rate, pitch):
                try:
                        mp3_data = asyncio.run(synthesize(text, voice, rate, pitch))
                        with self.lock:
                                self.last_status = f"Playing on {device_name}"
                        play_to_device(mp3_data, device_id)
                        with self.lock:
                                self.last_status = "Done"
                except Exception as exc:
                        with self.lock:
                                self.last_error = str(exc)
                                self.last_status = "Error"
                finally:
                        with self.lock:
                                self.is_busy = False


def build_web_handler(service):
        class WebHandler(BaseHTTPRequestHandler):
                def _send_json(self, payload, status=200):
                        body = json.dumps(payload).encode("utf-8")
                        self.send_response(status)
                        self.send_header("Content-Type", "application/json; charset=utf-8")
                        self.send_header("Content-Length", str(len(body)))
                        self.end_headers()
                        self.wfile.write(body)

                def _read_json(self):
                        content_len = int(self.headers.get("Content-Length", "0"))
                        if content_len <= 0:
                                return {}
                        raw = self.rfile.read(content_len)
                        try:
                                return json.loads(raw.decode("utf-8"))
                        except json.JSONDecodeError:
                                return {}

                def do_GET(self):
                        if self.path == "/":
                                html = """<!doctype html>
<html>
<head>
    <meta charset=\"utf-8\" />
    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\" />
    <title>TTS Virtual Mic</title>
    <style>
        :root { color-scheme: light; }
        body { font-family: Segoe UI, Tahoma, sans-serif; margin: 0; background: #f3f6fb; color: #1d2733; }
        .wrap { max-width: 760px; margin: 16px auto; padding: 16px; }
        .card { background: #ffffff; border-radius: 14px; box-shadow: 0 8px 22px rgba(20,40,80,.08); padding: 16px; }
        h1 { margin: 0 0 12px; font-size: 1.35rem; }
        .grid { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; }
        label { font-size: .9rem; color: #4d5c6e; margin-bottom: 4px; display: block; }
        input, select, textarea, button { font: inherit; }
        textarea, input, select { width: 100%; box-sizing: border-box; border: 1px solid #c8d4e5; border-radius: 10px; padding: 10px; background: #fff; }
        textarea { min-height: 140px; resize: vertical; }
        .buttons { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; margin-top: 12px; }
        button { border: none; border-radius: 10px; padding: 11px; font-weight: 600; background: #0a66c2; color: #fff; }
        button.secondary { background: #2a7f62; }
        button:disabled { opacity: .6; }
        .status { margin-top: 12px; font-size: .95rem; }
        .hint { margin-top: 8px; font-size: .85rem; color: #5b6a7c; }
    </style>
</head>
<body>
    <div class=\"wrap\">
        <div class=\"card\">
            <h1>TTS Virtual Mic Control</h1>

            <div class=\"grid\">
                <div>
                    <label for=\"voice\">Voice</label>
                    <select id=\"voice\"></select>
                </div>
                <div>
                    <label for=\"device\">Output Device</label>
                    <select id=\"device\"></select>
                </div>
                <div>
                    <label for="rate">Rate (0.25-4.0)</label>
                    <input id="rate" placeholder="1.0" type="number" min="0.25" max="4.0" step="0.1" />
                </div>
                <div>
                    <label for="pitch">Pitch (-20 to 20)</label>
                    <input id="pitch" placeholder="0" type="number" min="-20" max="20" step="1" />
                </div>
            </div>

            <div style=\"margin-top: 12px;\">
                <label for=\"text\">Text to speak</label>
                <textarea id=\"text\">Type text here and tap Speak.</textarea>
            </div>

            <div class=\"buttons\">
                <button id=\"speakBtn\">Speak</button>
                <button id=\"testBtn\" class=\"secondary\">Test Output</button>
            </div>

            <div id=\"status\" class=\"status\">Ready.</div>
            <div class=\"hint\">Open this page from your tablet Firefox while this app runs on the PC.</div>
        </div>
    </div>

    <script>
        const voiceEl = document.getElementById('voice');
        const deviceEl = document.getElementById('device');
        const rateEl = document.getElementById('rate');
        const pitchEl = document.getElementById('pitch');
        const textEl = document.getElementById('text');
        const statusEl = document.getElementById('status');
        const speakBtn = document.getElementById('speakBtn');
        const testBtn = document.getElementById('testBtn');

        async function postJson(url, payload) {
            const res = await fetch(url, {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify(payload || {})
            });
            return res.json();
        }

        function setBusy(busy) {
            speakBtn.disabled = busy;
            testBtn.disabled = busy;
        }

        function fillState(state) {
            voiceEl.innerHTML = '';
            for (const v of state.voices) {
                const opt = document.createElement('option');
                opt.value = v.id;
                opt.textContent = v.label;
                voiceEl.appendChild(opt);
            }

            deviceEl.innerHTML = '';
            for (const d of state.devices) {
                const opt = document.createElement('option');
                opt.value = d;
                opt.textContent = d;
                deviceEl.appendChild(opt);
            }

            voiceEl.value = state.voice;
            deviceEl.value = state.device;
            rateEl.value = state.rate;
            pitchEl.value = state.pitch;

            setBusy(state.busy);
            statusEl.textContent = state.error ? `${state.status}: ${state.error}` : state.status;
        }

        async function syncState() {
            const state = await (await fetch('/api/state')).json();
            fillState(state);
        }

        async function applySettings() {
            await postJson('/api/settings', {
                voice: voiceEl.value,
                device: deviceEl.value,
                rate: rateEl.value,
                pitch: pitchEl.value,
            });
        }

        voiceEl.addEventListener('change', applySettings);
        deviceEl.addEventListener('change', applySettings);
        rateEl.addEventListener('change', applySettings);
        pitchEl.addEventListener('change', applySettings);

        speakBtn.addEventListener('click', async () => {
            await applySettings();
            const result = await postJson('/api/speak', { text: textEl.value });
            statusEl.textContent = result.message || 'Speak requested.';
            await syncState();
        });

        testBtn.addEventListener('click', async () => {
            await applySettings();
            const result = await postJson('/api/test', {});
            statusEl.textContent = result.message || 'Test requested.';
            await syncState();
        });

        syncState();
        setInterval(syncState, 1200);
    </script>
</body>
</html>"""
                                body = html.encode("utf-8")
                                self.send_response(200)
                                self.send_header("Content-Type", "text/html; charset=utf-8")
                                self.send_header("Content-Length", str(len(body)))
                                self.end_headers()
                                self.wfile.write(body)
                                return

                        if self.path == "/api/state":
                                service.refresh_devices()
                                self._send_json(service.get_state())
                                return

                        self._send_json({"error": "Not found"}, status=404)

                def do_POST(self):
                        if self.path == "/api/settings":
                                payload = self._read_json()
                                service.refresh_devices()
                                service.set_settings(payload)
                                self._send_json({"ok": True})
                                return

                        if self.path == "/api/speak":
                                payload = self._read_json()
                                text = (payload.get("text") or "").strip()
                                if not text:
                                        self._send_json({"ok": False, "message": "Text is empty"}, status=400)
                                        return
                                service.refresh_devices()
                                ok, message = service.start_speak(text)
                                self._send_json({"ok": ok, "message": message}, status=200 if ok else 409)
                                return

                        if self.path == "/api/test":
                                service.refresh_devices()
                                ok, message = service.start_speak(TEST_MESSAGE)
                                self._send_json({"ok": ok, "message": message}, status=200 if ok else 409)
                                return

                        self._send_json({"error": "Not found"}, status=404)

                def log_message(self, fmt, *args):
                        return

        return WebHandler


def get_local_ip():
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
                sock.connect(("8.8.8.8", 80))
                return sock.getsockname()[0]
        except OSError:
                return "127.0.0.1"
        finally:
                sock.close()


def web_main():
        host = "0.0.0.0"
        port = 8765
        service = WebTTSService()

        if not service.devices:
                print("ERROR: No audio output devices found.")
                sys.exit(1)

        server = ThreadingHTTPServer((host, port), build_web_handler(service))
        local_ip = get_local_ip()

        print("=" * 60)
        print("  TTS Virtual Mic Web Control")
        print("=" * 60)
        print(f"Open on PC:     http://127.0.0.1:{port}")
        print(f"Open on tablet: http://{local_ip}:{port}")
        print("Press Ctrl+C to stop.\n")

        try:
                server.serve_forever()
        except KeyboardInterrupt:
                pass
        finally:
                server.server_close()


def main():
    if "--cli" in sys.argv:
        cli_main()
        return
    if "--web" in sys.argv:
        web_main()
        return
    gui_main()


if __name__ == "__main__":
    main()
