# ESP32 WiFi Text-to-Speech Device — v2.0.0

An ESP32-WROOM-32 device that serves a web page over WiFi, accepts text input from a browser, calls the Google Cloud Text-to-Speech API with premium Neural2 voices, decodes the returned MP3 audio, and outputs it through a PCM5102A I2S DAC. The analog output is attenuated via a voltage divider to mic-level and fed into a Samsung tablet's 3.5mm TRRS mic jack, allowing the tablet to "hear" the synthesised speech.

Built with PlatformIO and the Arduino framework. Single source file (`src/main.cpp`). Companion PC app (Python) provides GUI, CLI, and web-based TTS control with the same high-quality voices.

## Release 2.0.0 Highlights

**Voice Quality Upgrade:**
- **PC app** now uses Google Cloud Text-to-Speech **Neural2 voices** (previously Edge TTS)
  - 8 premium voices: US (Aria, Christopher, Joanna, Joseph) + UK (Amy, Brian, Diana, Oliver)
  - Authentic, natural voice quality matching Google Devices (Home, Nest, etc.)
  - Unified voice experience: both ESP32 and PC app now use the same premium voices
- Rate control: 0.25× to 4.0× speed (1.0 = normal)
- Pitch control: −20 to +20 semitones

**Previous improvements (v1.1.0):**
- Built-in hardware validation with **Test Tone** (440Hz, no internet required)
- GPIO13 status LED behaviour (ready, processing, WiFi connect, reset warning)
- GPIO35 switch actions: short press (speaks "System ready"), long press (hardware reset)
- Improved TTS robustness: tolerant `audioContent` extraction, WiFi power-save disabled, TTS timing logs

## Network Addressing (No Serial Monitor Needed)

By default, home routers assign a dynamic IP (DHCP), which can change. This firmware now supports two user-friendly ways to avoid that problem:

1. Stable hostname via mDNS (recommended):
  - Open `http://esp32-tts.local` instead of an IP address.
  - This usually works automatically on Android, macOS, iOS, and many Windows setups.

2. Optional fixed static IP:
  - In `src/main.cpp`, set `USE_STATIC_IP = true` and adjust:
    - `STATIC_IP`
    - `STATIC_GW`
    - `STATIC_MASK`
    - `STATIC_DNS1` / `STATIC_DNS2`
  - Choose an unused IP in your router's subnet (for example `192.168.1.220`).

Best long-term option: create a DHCP reservation in your router for the ESP32 MAC address. This keeps one fixed IP without hardcoding network values in firmware.

---

## Prerequisites

### Software

1. **VS Code** — [https://code.visualstudio.com/](https://code.visualstudio.com/)
2. **PlatformIO IDE extension** — install from the VS Code Extensions marketplace (search "PlatformIO IDE"). This installs the PlatformIO CLI, toolchains, and the ESP32 platform automatically on first build.
3. **USB driver** — the ESP32 DevKit v1 uses a CH340 USB-Serial chip. Windows 10/11 usually installs the driver automatically; if COM7 doesn't appear, download the driver from [http://www.wch-ic.com/downloads/CH341SER_EXE.html](http://www.wch-ic.com/downloads/CH341SER_EXE.html).
4. **Google Cloud account** — for the Text-to-Speech API key (see [API Key Setup](#api-key-setup) below).

### Tools

- Soldering iron + solder (for PCM5102A solder bridge pads and wiring)
- Wire strippers / hookup wire
- Multimeter (helpful for verifying voltage divider output)

---

## Hardware

| Component | Role | Notes |
|-----------|------|-------|
| ESP32-WROOM-32 (DevKit v1) | MCU, WiFi, web server, I2S master | CH340 USB-Serial; Micro-USB port |
| GY-PCM5102 (PCM5102A DAC) | 32-bit I2S stereo DAC | Amazon UK, ~£5.49; includes 3.5mm TRS cable |
| LED (GPIO13) | Visual system status indicator | Active HIGH |
| Momentary switch (GPIO35) | Local trigger/reset input | Press-to-GND logic |
| 10KΩ resistor (switch pull-up) | Pulls GPIO35 HIGH at idle | To 3.3V |
| 10µF electrolytic capacitor | DC-blocking on DAC output | + side toward PCM5102A |
| 10KΩ resistor | Upper leg of voltage divider | |
| 100Ω resistor | Lower leg of voltage divider | |
| 3.5mm panel-mount TRRS socket (4-pole) | Mic output to tablet through box wall | Must be 4-pole TRRS, not 3-pole TRS |
| 3.5mm panel-mount TRS socket (3-pole) | Monitoring output for earbuds | Can be TRS or TRRS; TRRS plugs work in either |
| 3.5mm TRRS male-to-male cable | Connects mic panel socket to tablet | Must be 4-pole TRRS to carry mic signal |
| 3.5mm TRS male-to-male cable | Connects PCM5102A jack to monitoring panel socket | Short cable, stays inside the box; included with PCM5102A |
| Project box (~100×60×25mm) | Enclosure for all electronics | |
| Micro-USB OTG adapter (Micro-USB male → USB-A female) | Lets tablet act as USB host | Plugs into tablet's Micro-USB port |
| USB-A to Micro-USB cable | Powers ESP32 from the OTG adapter | Standard phone charging cable |

### Wiring: ESP32 → PCM5102A

```
ESP32 GPIO 27  ──>  BCK   (bit clock)
ESP32 GPIO 26  ──>  DIN   (data in)
ESP32 GPIO 25  ──>  LCK   (word select / LRCLK)
ESP32 3.3V     ──>  VIN
ESP32 GND      ──>  GND
```

### Wiring: LED + Switch

```
GPIO13  ──> LED anode (with suitable series resistor), LED cathode ──> GND

3.3V ──[10KΩ]──┬── GPIO35
          │
        [switch]
          │
          GND
```

- GPIO35 is input-only on ESP32 and has no internal pull-up/down.
- In this firmware, **LOW = pressed** (switch shorts GPIO35 to GND).

### LED Flash Modes (GPIO13)

| Mode | Pattern | Meaning |
|------|---------|---------|
| Idle / Ready | Solid ON | System is connected and waiting for input |
| WiFi connect | Slow blink (~500 ms toggle) | Device is trying to join WiFi during startup |
| Processing / Speaking | Fast blink (~150 ms toggle) | TTS request is in progress or audio is playing |
| Reset warning | Very fast blink (~50 ms toggle) | Switch is being held for long-press hardware reset |

**Switch interaction (GPIO35):**
- Short press (< 2s): Speaks "System ready" to confirm end-to-end operation
- Long press (≥ 2s): Hardware reset (LED enters reset-warning flash first)

### Solder Bridge Pads (rear of GY-PCM5102 board)

SCK, XSMT, FMT, and DEMP are **not** exposed as header pins. They are configured via solder bridge pads on the back of the PCB, labeled H1L–H4L. Each pad has an **H** (high / 3.3V) side and an **L** (low / GND) side. Bridge to the correct side with a small solder blob:

| Pad | Signal | Bridge to | Effect |
|-----|--------|-----------|--------|
| **H1L** | SCK (system clock) | **L** (GND) | Use DAC's internal clock |
| **H2L** | FMT (audio format) | **L** (GND) | Standard I2S format |
| **H3L** | XSMT (soft mute) | **H** (3.3V) | DAC unmuted — **audio will not play if this is left low or open** |
| **H4L** | DEMP (de-emphasis) | **L** (GND) | De-emphasis off (correct for speech) |

> **Tip:** Some boards ship with these already bridged to sensible defaults. Check each pad — if H3L (XSMT) is bridged to **L** or unbridged, you will get silence.

### PCM5102A Audio Outputs

The GY-PCM5102 board provides **two ways** to access the analog audio output:

#### Option A: Header pads (L, G, R, G)

The output header on the board edge has four pads:

| Pad | Signal |
|-----|--------|
| **L** | Left channel analog output |
| **G** | Ground |
| **R** | Right channel analog output |
| **G** | Ground |

Solder wires directly to **L** (or **R**) and **G** to feed the voltage divider (see below). This leaves the built-in 3.5mm jack free for monitoring.

#### Built-in 3.5mm headphone jack → monitoring socket

The board also has a **3.5mm TRS (3-pole) headphone jack** that carries the same line-level stereo signal. A short TRS cable (included with the board) connects this jack to a **panel-mount TRS socket** on the enclosure wall. Plug earbuds into the panel socket to monitor the audio output.

> **Note:** The monitoring socket outputs **line-level** audio — noticeably louder than a phone's headphone output, but not harmful. Earbuds with a TRRS plug work fine in a TRS socket.

### Analogue Output → Tablet Mic Jack

The header pads feed the voltage divider circuit. The audio must pass through a DC-blocking capacitor and voltage divider before reaching the tablet's TRRS mic input:

```
PCM5102A L pad ──[10µF cap]──[10KΩ]──┬──[100Ω]──> PCM5102A G pad
                      +              │
                                     └──────────> TRRS Sleeve (mic in)

PCM5102A G pad ──────────────────────────────────> TRRS Ring2 (ground)
```

- The **10µF electrolytic capacitor** blocks DC offset. Orient with **+** side toward the PCM5102A.
- The **10KΩ** is in **series** between the signal and the tap point. The **100Ω** is the lower leg to ground. The output (mic) is taken from the junction between them: Vout = Vin × 100 / (10000 + 100) ≈ Vin / 101.
- This attenuates the ~2 Vpp line-level signal down to ~20 mVpp (mic level).
- Use a **multimeter** to verify ~20 mV AC across the 100Ω resistor while audio is playing.

### TRRS Cable to Tablet

You need a **3.5mm TRRS (4-pole) plug** to connect to the Samsung tablet's headset jack. TRRS carries signals in **both directions** — the tablet sends headphone audio out, and receives microphone audio in:

| Pin | Direction | Signal | This project |
|-----|-----------|--------|--------------|
| **Tip** | Tablet → headphone | Left audio out | Not used |
| **Ring1** | Tablet → headphone | Right audio out | Not used |
| **Ring2** | Shared | Ground | Connect to PCM5102A **G** / TRS Sleeve |
| **Sleeve** | External → tablet | Microphone in | Connect voltage divider output here |

> The pinout above is the **CTIA standard** used by Samsung — and most Android devices. Older Sony/Nokia devices use OMTP (mic and ground swapped); check your device if it's not Samsung.

### Enclosure Build

The ESP32, PCM5102A, and voltage divider circuit fit inside a small project box. Three connections exit the enclosure:

1. **USB cable** — powers the ESP32 from the tablet's Micro-USB port (via USB OTG adapter)
2. **TRRS panel socket** — mic output, connects via TRRS cable to the tablet's headset jack
3. **TRS panel socket** — monitoring output, plug in earbuds to hear the audio

```
┌──────────────────────────────────────────────────────────────────┐
│  ENCLOSURE                                                      │
│                                                                  │
│  ┌──────────┐  I2S   ┌────────────┐                              │
│  │  ESP32   │───────▶│ PCM5102A   │                              │
│  │  (5V in, │  3.3V  │  (DAC)     │                              │
│  │  3.3V    │───────▶│            │                              │
│  │  reg)    │  VIN   └──┬─────┬───┘                              │
│  └────┬─────┘           │     │                                  │
│       │          header pads  3.5mm jack                          │
│       │          (L + G)      │                                   │
│       │              │        TRS cable (included)                │
│       │              │        │                           ┌──────┐│
│       │              │        └──────────────────────────▶│TRS   ││
│       │              │           (line-level monitoring)  │panel ││
│       │              │                                    │socket││
│       │          L wire   G wire                          └──┬───┘│
│       │            │        │                                │    │
│       │        [10µF cap]   │                             earbuds │
│       │            │        │                                     │
│       │        [10KΩ]       │                                     │
│       │            │        │                                     │
│       │            ├─[100Ω]─┤                                     │
│       │            │        │                             ┌──────┐│
│       │            │        └────────────────────────────▶│TRRS  ││
│       │            └────────────────────────────────────▶│panel ││
│       │                          (to Sleeve/mic)         │socket││
│       │                          (to Ring2/GND)          └──┬───┘│
│  ─────┼─────────────────────────────────────────────────────┼────│
│       │ USB-A to                                            │    │
│       │ Micro-USB                                     TRRS cable │
└───────┼─────────────────────────────────────────────────────┼────┘
        │                                                     │
        ▼                                                     ▼
  ┌────────────┐                                      ┌────────────┐
  │ Micro-USB  │                                      │ 3.5mm jack │
  │ OTG adapter│                                      └────────────┘
  └────┬───────┘                                      Samsung Tablet
       │
  ┌────┴───────┐
  │ Micro-USB  │
  └────────────┘
  Samsung Tablet
```

#### Signal flow (inside the box)

```
PCM5102A
  ├── Header pads (L + G) ──▶ Voltage divider ──▶ TRRS panel socket (tablet mic)
  │       │
  │   L pad ──[10µF cap +→−]──[10KΩ]──┬──[100Ω]──▶ G pad (GND)
  │                                    │
  │                                    └──────────▶ TRRS socket: Sleeve (mic in)
  │   G pad ──────────────────────────────────────▶ TRRS socket: Ring2 (ground)
  │
  └── Built-in 3.5mm jack ──TRS cable──▶ TRS panel socket (earbuds monitoring)
```

#### Power chain

```
Samsung tablet Micro-USB  →  Micro-USB OTG adapter  →  USB-A to Micro-USB cable  →  ESP32
                                                                              │
                                         5V from USB ──▶ AMS1117 regulator ──▶ 3.3V
                                                                              │
                                                          ESP32 3.3V pin ──▶ PCM5102A VIN
```

- The tablet supplies **5V** via USB OTG. The ESP32 DevKit has an onboard **AMS1117 voltage regulator** that converts 5V → 3.3V.
- The PCM5102A runs on **3.3V** from the ESP32's 3.3V pin — **not** directly from USB 5V.
- The ESP32 draws ~150–250mA during WiFi activity. This will slowly drain the tablet battery — consider keeping the tablet plugged into its charger during extended use.
- **Note:** When the OTG adapter is plugged into the tablet's only Micro-USB port, you cannot charge the tablet at the same time. For extended use, charge the tablet beforehand or use a powered USB OTG hub.
- If the tablet doesn't supply power, check that USB OTG is enabled in **Settings → Developer options** (or **Settings → Connections → USB OTG** on some Samsung models).

---

## Software Stack

### External Libraries

| Library | Version | Source | Purpose |
|---------|---------|--------|---------|
| **ESP8266Audio** | ≥1.9.7 | `earlephilhower/ESP8266Audio` | MP3 decoder (`AudioGeneratorMP3`), I2S output (`AudioOutputI2S`), in-memory source (`AudioFileSourcePROGMEM`) |
| **ArduinoJson** | ≥6.21.0 | `bblanchon/ArduinoJson` | JSON serialisation for the TTS API request and parsing the `/speak` POST body |

### Built-in ESP32/Arduino Libraries

| Library | Purpose |
|---------|---------|
| `WiFi.h` | Station-mode WiFi connection |
| `WebServer.h` | Synchronous HTTP server (port 80) |
| `WiFiClientSecure.h` | HTTPS client for Google Cloud TTS API |
| `mbedtls/base64.h` | Base64 decoding of the TTS audio response |

### Build Configuration (platformio.ini)

- **Board**: `esp32dev` (ESP32-WROOM-32)
- **Framework**: Arduino
- **Serial monitor**: 115200 baud
- **Build flag**: `-DCORE_DEBUG_LEVEL=3` (info-level debug output)

---

## Programme Flow

### 1. Startup (`setup()`)

```
Serial.begin(115200)
Print banner with firmware version
Connect to WiFi (SSID: configured, STA mode)
  └─ 20-second timeout → ESP.restart()
Disable WiFi power saving for lower latency
Register HTTP routes:
  GET  /        → handleRoot()    — serve HTML page
  POST /speak   → handleSpeak()   — accept TTS request
  POST /volume  → handleVolume()  — update playback gain
  POST /tone    → handleTone()    — 440Hz hardware test tone
  GET  /status  → handleStatus()  — playback progress
Start web server on port 80
Print IP address
```

### 2. Main Loop (`loop()`)

```
loop():
  server.handleClient()          ← service any pending HTTP request
  
  if MP3 decoder is running:
      call mp3->loop()           ← feed next audio samples to I2S
      if decoder returns false:
          stopPlayback()         ← free all audio resources
  
  else if pendingTTS flag is set:
      clear flag
      call startTTS()            ← initiate API call
```

The MP3 decoder runs cooperatively — `mp3->loop()` is called every iteration to push decoded PCM samples to the I2S peripheral. During TTS download (a few seconds), the web server is blocked.

### 3. TTS API Call (`startTTS()`)

```
stopPlayback()                     ← free any previous audio resources
Connect to texttospeech.googleapis.com:443 (TLS, no cert verification)
Extract language code from voice name (e.g. "en-US-Standard-F" → "en-US")
Build JSON request:
  {
    "input": { "text": "..." },
    "voice": { "languageCode": "en-US", "name": "en-US-Standard-F" },
    "audioConfig": {
      "audioEncoding": "MP3",
      "speakingRate": 1.0,
      "pitch": 0.0
    }
  }
POST /v1/text:synthesize?key=API_KEY
Wait for response (20s timeout)
Read HTTP headers (detect chunked vs content-length body framing)
Stream response body and extract base64 `audioContent` with tolerant JSON scanning
Decode base64 → binary MP3 using mbedtls_base64_decode()
  └─ Two-pass: first call gets required size, malloc, second call decodes
Build audio pipeline:
  AudioFileSourcePROGMEM(buffer, length)
  AudioOutputI2S(port 0, pins 27/25/26, gain set by /volume)
  AudioGeneratorMP3 → begin()
Set audioPlaying = true
Record playbackStartMs and estimate duration from buffer size (~32kbps)
```

### 4. Web Interface

The HTML page is stored in `PROGMEM` as a raw string literal (~3KB). At serve time, `%FW_VER%`, `%BUILD_DATE%`, and `%BUILD_TIME%` placeholders are replaced with build metadata.

**UI elements:**
- Text area for message input (Enter key triggers speak)
- Volume slider (0.0 to 4.0 gain)
- Speech rate slider (0.5× – 2.0×)
- Pitch slider (-10 to +10 semitones)
- Voice selector dropdown (18 voices: 10 female, 8 male, US and UK, Standard and WaveNet)
- Test Tone button (local 440Hz verification)
- Status indicator (info/success/error states)
- Canvas waveform display with progress cursor

**JavaScript flow:**
1. User types text → presses Enter or clicks Speak
2. `speak()` sends `POST /speak` with JSON body `{ text, rate, pitch, voice }`
3. On success, starts canvas waveform animation
4. Polls `GET /status` every 300ms — returns `{ playing, elapsed, duration }`
5. Cursor position = `elapsed / duration` across the canvas width
6. When `playing` becomes false, animation stops with cursor at right edge

### 5. Audio Resource Management (`stopPlayback()`)

All audio objects are dynamically allocated and must be freed after each utterance:
- `AudioGeneratorMP3` — stop and delete
- `AudioFileSourcePROGMEM` — delete
- `AudioOutputI2S` — delete
- `audioBuffer` — `free()`

This prevents memory leaks on the ESP32's limited 320KB RAM.

---

## API: Google Cloud Text-to-Speech

- **Endpoint**: `https://texttospeech.googleapis.com/v1/text:synthesize?key=API_KEY`
- **Method**: HTTPS POST with JSON body
- **Response**: JSON containing `audioContent` field with base64-encoded MP3
- **Free tier**: 1 million characters/month (Standard voices), 4 million first 12 months (WaveNet)
- **Console**: https://console.cloud.google.com/apis/credentials

**Available voices (configured in UI):**

| Voice | Type | Gender | Accent |
|-------|------|--------|--------|
| en-US-Standard-C/E/F | Standard | Female | US |
| en-US-Wavenet-C/F | WaveNet (HQ) | Female | US |
| en-US-Standard-A/B/D | Standard | Male | US |
| en-US-Wavenet-A/B | WaveNet (HQ) | Male | US |
| en-GB-Standard-A/C | Standard | Female | UK |
| en-GB-Wavenet-A/C | WaveNet (HQ) | Female | UK |
| en-GB-Standard-B/D | Standard | Male | UK |
| en-GB-Wavenet-B/D | WaveNet (HQ) | Male | UK |

---

## First-Time Setup

### 1. API Key Setup

You need a Google Cloud Text-to-Speech API key. The free tier allows **1 million characters/month** (Standard voices) or **4 million characters in the first 12 months** (WaveNet voices).

1. Go to [Google Cloud Console](https://console.cloud.google.com/) and sign in (or create a free account).
2. Create a new project (e.g. "ESP32-TTS") or select an existing one.
3. Navigate to **APIs & Services → Library**.
4. Search for **"Cloud Text-to-Speech API"** and click **Enable**.
5. Navigate to **APIs & Services → Credentials**.
6. Click **+ Create Credentials → API key**.
7. Copy the generated key — you'll add it to local config files next.
8. *(Recommended)* Click **Edit API key**, then under **API restrictions** select **Restrict key** and choose only **Cloud Text-to-Speech API**. This limits damage if the key is ever leaked.

### 2. Configure Firmware

Copy `src/secrets.example.h` to `src/secrets.h`, then edit the copied file with your WiFi and Google TTS credentials:

```cpp
// src/secrets.h
#pragma once

const char *WIFI_SSID = "YourWiFiName";
const char *WIFI_PASSWORD = "YourWiFiPassword";
const char *TTS_API_KEY = "AIza...";
```

`src/secrets.h` is gitignored, so your real credentials stay on your machine and out of the repository.

> **Security:** The API key is still compiled into the firmware binary. Keep the binary private, and never commit `src/secrets.h`.

### 3. Build & Upload

Plug the ESP32 into USB (it should appear as COM7 with the CH340 driver).

```bash
# Build firmware (PlatformIO downloads all dependencies on first run)
pio run

# Upload to ESP32
pio run --target upload

# Monitor serial output (see IP address, debug messages)
pio device monitor --port COM7 --baud 115200
```

Or use the PlatformIO toolbar buttons in VS Code (Build ✓, Upload →, Monitor 🔌).

### 4. Connect

1. After upload, open the serial monitor — the ESP32 prints its IP address (e.g. `192.168.1.42`).
2. Open that IP in a browser on any device connected to the same WiFi network.
3. Type text and press Enter — the ESP32 calls the Google TTS API, decodes the MP3, and plays it through the DAC.
4. Audio appears at the tablet's mic input via the TRRS cable.

---

## Memory Usage (v1.1.0)

```
RAM:   14.1%  (46,228 / 327,680 bytes)
Flash: 81.5%  (1,068,229 / 1,310,720 bytes)
```

---

## Project Structure

```
Text_to_Speech/
├── .gitignore
├── platformio.ini          # PlatformIO build configuration
├── README.md               # This file
├── src/
│   ├── main.cpp            # Complete ESP32 firmware (~645 lines)
│   └── secrets.example.h   # Copy to secrets.h locally and add credentials
└── pc_app/                 # Standalone desktop TTS tool (see below)
  ├── local_config.example.json
    ├── tts_mic.py
    └── requirements.txt
```

---

## PC App — Desktop TTS Virtual Microphone

The `pc_app/` folder contains a standalone Python tool that performs TTS **on a PC** and routes the audio to a virtual audio cable (e.g. VB-Cable) so that other applications (Discord, Zoom, etc.) hear it as microphone input.

**v2.0.0 upgrade:** Now uses **Google Cloud Text-to-Speech Neural2 voices** (premium quality, same as the ESP32 device). The app provides three interfaces:
1. **GUI** — graphical user interface with voice/rate/pitch controls
2. **CLI** — command-line interface for scripting
3. **Web** — browser-based control (mobile-friendly)

All three use the same high-quality Neural2 voices.

### PC App Requirements

- Python 3.9+
- **Google Cloud account** — use the same API key as the ESP32, but keep it in `pc_app/local_config.json` or the `GOOGLE_TTS_API_KEY` environment variable
- **VB-Cable** (free) or **VoiceMeeter** for the virtual mic device — [https://vb-audio.com/Cable/](https://vb-audio.com/Cable/)
- **ffmpeg** on PATH (needed by pydub for MP3 playback) — [https://ffmpeg.org/download.html](https://ffmpeg.org/download.html)

### PC App Setup & Usage

```bash
cd pc_app
pip install -r requirements.txt
copy local_config.example.json local_config.json
```

Then edit `pc_app/local_config.json` and add your Google Cloud API key:

```json
{
  "google_tts_api_key": "AIza..."
}
```

`pc_app/local_config.json` is gitignored, so it will not be pushed to GitHub.

**Run GUI (default):**
```bash
python tts_mic.py
```
Opens a graphical window. Select voice, adjust rate (0.25–4.0) and pitch (−20 to +20), then click **Speak**.

**Run CLI:**
```bash
python tts_mic.py --cli
```
Command-line interface. Type text and press Enter. Commands: `/voice`, `/rate <val>`, `/pitch <val>`, `/device`, `/settings`, `/help`, `/quit`.

**Run Web Interface:**
```bash
python tts_mic.py --web
```
Opens HTTP server on `http://127.0.0.1:8765` (or your PC's IP for remote access). Mobile-friendly browser interface.

---

## Assembly Tips

- **Test the tablet's mic input first:** Before building the enclosure, verify that the tablet's 3.5mm jack accepts microphone input (i.e. it's a 4-pole TRRS socket, not headphone-only TRS). Plug in any **headset with a built-in mic** (e.g. earbuds that came with a phone), open a voice recorder app, and speak. If the recording picks up audio from the headset mic, the jack supports TRRS mic input and will work for this project. If it only uses the tablet's built-in mic, the jack may be TRS-only (headphone output only) and you'll need a USB audio adapter instead.
- **Capacitor orientation:** The **+** leg of the 10µF electrolytic cap connects to the PCM5102A side (L pad or TRS Tip). The **−** leg (marked with a stripe on the cap body) faces toward the voltage divider / TRRS plug.
- **Voltage divider check:** With audio playing, measure AC voltage across the 100Ω resistor with a multimeter — you should see roughly 15–25 mV. If you see hundreds of millivolts, double-check the resistor values (10KΩ and 100Ω, not 10KΩ and 1KΩ).
- **No audio?** Check these in order:
  1. H3L (XSMT) solder bridge — must be bridged to **H** (3.3V side)
  2. Serial monitor — confirm the ESP32 connected to WiFi and received a TTS response
  3. TRRS plug — confirm Sleeve (mic) and Ring2 (ground) are connected, not swapped
  4. Try connecting headphones directly to the PCM5102A's built-in 3.5mm jack to verify the DAC is outputting audio

---

## Known Limitations

- **Certificate verification disabled** — `client.setInsecure()` skips TLS cert validation for simplicity. For production use, pin the Google root CA or use a certificate bundle.
- **Synchronous web server** — during the TTS API call (typically 2–5 seconds), the web interface is unresponsive. The `/speak` handler returns immediately, but `startTTS()` blocks in `loop()`.
- **Waveform is cosmetic** — the canvas animation shows a synthetic sine-based waveform, not real audio data. The cursor tracks estimated playback progress based on MP3 buffer size.
- **Single utterance** — submitting new text while audio is playing will queue it (via `pendingTTS` flag) and play after the current utterance finishes.
- **API key in source** — the Google Cloud API key is hardcoded. Keep the repository private or use environment variables for deployment.
