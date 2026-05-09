// ============================================================
// ESP32 WiFi Text-to-Speech Device  —  v2.0.0
// Uses Google Cloud TTS API with multiple voice options
// Output feeds into a tablet's mic jack via PCM5102A DAC
// ============================================================

#define FIRMWARE_VERSION "2.0.0"

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <WiFiClientSecure.h>
#include <esp_task_wdt.h>
#include <driver/i2s.h>
#include <Preferences.h>
#include <math.h>
#include <ArduinoJson.h>
#include <mbedtls/base64.h>

#include <AudioFileSourcePROGMEM.h>
#include <AudioGeneratorMP3.h>
#include <AudioOutputI2S.h>
#include "secrets.h"

// ============================================================
// Configuration — local secrets live in src/secrets.h (gitignored)
// ============================================================

// Network access options:
// 1) mDNS hostname (recommended): http://esp32-tts.local
// 2) Optional static IP below (set USE_STATIC_IP to true)
const char *DEVICE_HOSTNAME = "esp32-tts";
constexpr bool USE_STATIC_IP = true;
IPAddress STATIC_IP(192, 168, 68, 220);
IPAddress STATIC_GW(192, 168, 68, 1);
IPAddress STATIC_MASK(255, 255, 255, 0);
IPAddress STATIC_DNS1(8, 8, 8, 8);
IPAddress STATIC_DNS2(1, 1, 1, 1);

// Google Cloud Text-to-Speech API
// Free tier: 1 million characters/month (Standard), 4M first 12 months (WaveNet)
// Get your API key at: https://console.cloud.google.com/apis/credentials
const char *TTS_HOST    = "texttospeech.googleapis.com";
const int   TTS_PORT    = 443;

// ============================================================
// Audio output — PCM5102A external I2S DAC
// ============================================================
//
// The PCM5102A is a 32-bit I2S DAC that outputs line-level analog
// audio.  Much higher quality than the ESP32's internal 8-bit DAC.
//
// Wiring ESP32 → PCM5102A module:
//
//   ESP32 GPIO 27  -->  PCM5102A BCK   (bit clock)
//   ESP32 GPIO 26  -->  PCM5102A DIN   (data in)
//   ESP32 GPIO 25  -->  PCM5102A LCK   (word select / LRCLK)
//   ESP32 3.3V     -->  PCM5102A VIN
//   ESP32 GND      -->  PCM5102A GND
//
//   SCK, XSMT, FMT, DEMP are configured via solder bridge pads
//   on the rear of the GY-PCM5102 board (H1L–H4L):
//     H1L (SCK)  → bridge to L (GND)  = use internal clock
//     H2L (FMT)  → bridge to L (GND)  = I2S format
//     H3L (XSMT) → bridge to H (3.3V) = unmuted (REQUIRED for audio)
//     H4L (DEMP) → bridge to L (GND)  = de-emphasis off
//
// PCM5102A output pads:  L (left out), G (ground), R (right out), G (ground)
//
// Two audio paths out of the PCM5102A:
//
//   1) Header pads (L + G) → voltage divider → TRRS panel socket → tablet mic
//      Use either L or R pad (TTS audio is mono on both channels).
//
//      L pad ──[10µF cap, + toward PCM5102A]──[10KΩ]──┬──[100Ω]──> G pad (GND)
//                                                     │
//                                                     └──> TRRS panel socket: Sleeve (mic in)
//      G pad ──> TRRS panel socket: Ring2 (ground)
//
//      The 10µF cap blocks DC. The 10KΩ/100Ω divider
//      attenuates ~2Vpp line-level down to ~20mVpp (mic level).
//
//   2) Built-in 3.5mm jack → TRS cable → TRS panel socket → earbuds (monitoring)
//      Plug earbuds in to hear the audio. Line-level — louder than normal.
//
//   TRRS 3.5mm plug pinout (CTIA standard, used by Samsung):
//     Tip    = Left audio out  (tablet → headphone, not used)
//     Ring1  = Right audio out (tablet → headphone, not used)
//     Ring2  = Ground          (shared, connect to PCM5102A G)
//     Sleeve = Microphone in   (tablet ← our attenuated signal)
//
#define I2S_BCLK  27
#define I2S_LRC   25
#define I2S_DOUT  26

// ============================================================
// Hardware — LED and tactile switch
// ============================================================
// LED   : GPIO 13 (active HIGH)
// Switch: GPIO 35 (input-only pin — connect via external pull-up resistor
//         10 kΩ to 3.3V; switch connects pin to GND when pressed)
//
// Behaviour:
//   LED solid ON      = idle / ready
//   LED fast blink    = speaking / processing
//   LED slow blink    = connecting to WiFi
//
//   Switch short press (< 2 s) = speak "System ready" as a quick audio check
//   Switch long press  (≥ 2 s) = hardware reset  (LED blinks rapidly as warning)
#define LED_PIN    13
#define SWITCH_PIN 35

// Audio stream buffer size in bytes
#define AUDIO_BUFFER_SIZE 4096

// ============================================================
// HTML page served from PROGMEM
// ============================================================
static const char HTML_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1.0">
<title>ESP32 TTS v%FW_VER%</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;
  background:#1a1a2e;color:#eee;display:flex;justify-content:center;
  align-items:center;min-height:100vh;padding:20px}
.c{background:#16213e;border-radius:12px;padding:30px;max-width:500px;
  width:100%;box-shadow:0 8px 32px rgba(0,0,0,.3)}
h1{text-align:center;margin-bottom:4px;color:#e94560;font-size:1.6em}
.ver{text-align:center;margin-bottom:20px;color:#666;font-size:.75em}
label{display:block;margin:14px 0 4px;font-size:.9em;color:#aaa}
textarea{width:100%;height:100px;padding:10px;border:1px solid #333;
  border-radius:8px;background:#0f3460;color:#eee;font-size:1em;resize:vertical}
.sg{display:flex;align-items:center;gap:8px;margin-top:4px}
.sg span{font-size:.8em;color:#888;min-width:24px;text-align:center}
input[type=range]{flex:1;accent-color:#e94560}
.sv{min-width:36px;text-align:center;font-family:monospace;color:#e94560}
select{width:100%;padding:8px;border:1px solid #333;border-radius:8px;
  background:#0f3460;color:#eee;font-size:.95em}
button{padding:14px;border:none;border-radius:8px;
  background:#e94560;color:#fff;font-size:1.02em;font-weight:bold;
  cursor:pointer;transition:background .2s}
button:hover{background:#c73552}
button:disabled{background:#555;cursor:not-allowed}
.pr{margin-top:14px;background:#10263f;border:1px solid #2a3f5c;border-radius:8px;padding:10px}
.pr table{width:100%;border-collapse:collapse;font-size:.83em}
.pr th,.pr td{padding:6px 4px;border-bottom:1px solid #1e3450;text-align:left}
.pr th{color:#95d5b2;font-weight:600}
.pr tr:last-child td{border-bottom:none}
.pb{padding:6px 8px;font-size:.78em;border-radius:6px;background:#2f7f5f}
.pb:hover{background:#26684e}
.ba{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-top:22px}
.bt{background:#2f7f5f}
.bt:hover{background:#26684e}
#st{margin-top:14px;padding:10px;border-radius:8px;text-align:center;
  font-size:.9em;min-height:20px}
.ok{background:#1b4332;color:#95d5b2}
.err{background:#4a1526;color:#f4a0a0}
.info{background:#1a3a5c;color:#90caf9}
#wf{width:100%;height:80px;margin-top:16px;border-radius:8px;background:#0a0a1a;
  display:block}
</style>
</head>
<body>
<div class="c">
  <h1>&#128266; Text-to-Speech v%FW_VER%</h1>
  <div class="ver">v%FW_VER% &mdash; built %BUILD_DATE% %BUILD_TIME%</div>

  <label for="txt">Message</label>
  <textarea id="txt" placeholder="Enter text to speak..." onkeydown="if(event.key==='Enter'&&!event.shiftKey){event.preventDefault();speak();}"></textarea>

  <label>Volume: <span class="sv" id="vv">1.0</span></label>
  <div class="sg">
    <span>0</span>
    <input type="range" id="vol" min="0" max="4" step="0.1" value="1.0"
      oninput="document.getElementById('vv').textContent=parseFloat(this.value).toFixed(1);setVolume(this.value)">
    <span>4.0</span>
  </div>

  <label>Speech Rate: <span class="sv" id="rv">1.00</span></label>
  <div class="sg">
    <span>0.5</span>
    <input type="range" id="rate" min="0.5" max="4.0" step="0.01" value="1.00"
      oninput="document.getElementById('rv').textContent=parseFloat(this.value).toFixed(2)">
    <span>4.0</span>
  </div>

  <label>Pitch: <span class="sv" id="pv">0.0</span> semitones</label>
  <div class="sg">
    <span>-10</span>
    <input type="range" id="pitch" min="-10" max="10" step="0.1" value="0.0"
      oninput="document.getElementById('pv').textContent=parseFloat(this.value).toFixed(1)">
    <span>+10</span>
  </div>

  <label for="voice">Voice</label>
  <select id="voice">
    <optgroup label="Female (US)">
      <option value="en-US-Standard-C">Standard C (Female US)</option>
      <option value="en-US-Standard-E">Standard E (Female US)</option>
      <option value="en-US-Standard-F" selected>Standard F (Female US)</option>
      <option value="en-US-Wavenet-C">WaveNet C (Female US, HQ)</option>
      <option value="en-US-Wavenet-F">WaveNet F (Female US, HQ)</option>
    </optgroup>
    <optgroup label="Male (US)">
      <option value="en-US-Standard-A">Standard A (Male US)</option>
      <option value="en-US-Standard-B">Standard B (Male US)</option>
      <option value="en-US-Standard-D">Standard D (Male US)</option>
      <option value="en-US-Wavenet-A">WaveNet A (Male US, HQ)</option>
      <option value="en-US-Wavenet-B">WaveNet B (Male US, HQ)</option>
    </optgroup>
    <optgroup label="Female (UK)">
      <option value="en-GB-Standard-A">Standard A (Female UK)</option>
      <option value="en-GB-Standard-C">Standard C (Female UK)</option>
      <option value="en-GB-Wavenet-A">WaveNet A (Female UK, HQ)</option>
      <option value="en-GB-Wavenet-C">WaveNet C (Female UK, HQ)</option>
    </optgroup>
    <optgroup label="Male (UK)">
      <option value="en-GB-Standard-B">Standard B (Male UK)</option>
      <option value="en-GB-Standard-D">Standard D (Male UK)</option>
      <option value="en-GB-Wavenet-B">WaveNet B (Male UK, HQ)</option>
      <option value="en-GB-Wavenet-D">WaveNet D (Male UK, HQ)</option>
    </optgroup>
  </select>

  <div class="pr">
    <label style="margin:0 0 8px">Casual Presets</label>
    <table>
      <thead>
        <tr>
          <th>Mood</th>
          <th>Rate</th>
          <th>Pitch</th>
          <th>Action</th>
        </tr>
      </thead>
      <tbody>
        <tr>
          <td>Soft</td>
          <td>1.13</td>
          <td>+0.4</td>
          <td><button class="pb" onclick="applyPreset('soft')">Apply</button></td>
        </tr>
        <tr>
          <td>Chatty</td>
          <td>1.25</td>
          <td>+0.8</td>
          <td><button class="pb" onclick="applyPreset('chatty')">Apply</button></td>
        </tr>
        <tr>
          <td>Sleepy</td>
          <td>1.02</td>
          <td>0.0</td>
          <td><button class="pb" onclick="applyPreset('sleepy')">Apply</button></td>
        </tr>
        <tr>
          <td>Cheerful</td>
          <td>1.32</td>
          <td>+1.2</td>
          <td><button class="pb" onclick="applyPreset('cheerful')">Apply</button></td>
        </tr>
        <tr>
          <td>Neutral</td>
          <td>1.19</td>
          <td>+0.5</td>
          <td><button class="pb" onclick="applyPreset('neutral')">Apply</button></td>
        </tr>
        <tr>
          <td>Street</td>
          <td>1.10</td>
          <td>0.0</td>
          <td><button class="pb" onclick="applyPreset('street')">Apply</button></td>
        </tr>
      </tbody>
    </table>
  </div>

  <div class="ba">
    <button id="btn" onclick="speak()">Speak</button>
  </div>
  <div style="margin-top:10px">
    <button id="btnTone" style="width:100%;background:#6a3daa" onclick="testTone()">&#9835; Test Tone (440 Hz &mdash; no internet needed)</button>
  </div>
  <div id="st"></div>
  <canvas id="wf"></canvas>
</div>
<script>
var playing=false,anim=null,wfData=[];
var cv=null,cx=null;
var startTime=0,estDuration=0,serverPct=0;
function initWf(){
  cv=document.getElementById('wf');
  cx=cv.getContext('2d');
  cv.width=cv.offsetWidth*2;cv.height=cv.offsetHeight*2;
  cx.scale(2,2);
  if(playing){drawFrame();}else{drawIdle();}
}
function drawIdle(){
  var w=cv.offsetWidth,h=cv.offsetHeight;
  cx.clearRect(0,0,w,h);
  cx.strokeStyle='#1a3a5c';cx.lineWidth=1;
  cx.beginPath();cx.moveTo(0,h/2);cx.lineTo(w,h/2);cx.stroke();
}
function genWave(){
  wfData=[];var n=300;
  for(var i=0;i<n;i++){
    var v=0;
    v+=Math.sin(i*0.15)*0.3;
    v+=Math.sin(i*0.08)*0.5;
    v+=Math.sin(i*0.23)*0.2;
    v+=(Math.random()-0.5)*0.3;
    wfData.push(v);
  }
}
function getWaveY(i,w,h,mid){
  var n=wfData.length;
  var idx=(i/w*n)%n;
  var i0=Math.floor(idx)%n,i1=(i0+1)%n,f=idx-Math.floor(idx);
  var v=wfData[i0]*(1-f)+wfData[i1]*f;
  var pulse=Math.sin((i/w)*Math.PI);
  return mid+v*mid*0.75*pulse;
}
function getPct(){
  if(!playing)return 0;
  var elapsed=(Date.now()-startTime)/1000;
  var dur=estDuration>0?estDuration:10;
  var pct=elapsed/dur;
  if(serverPct>pct)pct=serverPct;
  return Math.min(pct,0.98);
}
function drawFrame(){
  if(!playing){drawIdle();return;}
  var w=cv.offsetWidth,h=cv.offsetHeight,mid=h/2;
  cx.clearRect(0,0,w,h);
  var pct=getPct();
  var cp=Math.max(1,Math.floor(pct*w));
  // Draw played portion (bright)
  if(cp>0){
    cx.strokeStyle='#e94560';cx.lineWidth=1.5;
    cx.beginPath();
    for(var i=0;i<cp;i++){
      var y=getWaveY(i,w,h,mid);
      if(i===0)cx.moveTo(i,y);else cx.lineTo(i,y);
    }
    cx.stroke();
  }
  // Draw unplayed portion (dim)
  if(cp<w){
    cx.strokeStyle='#3a1525';cx.lineWidth=1;
    cx.beginPath();
    for(var i=cp;i<w;i++){
      var y=getWaveY(i,w,h,mid);
      if(i===cp)cx.moveTo(i,y);else cx.lineTo(i,y);
    }
    cx.stroke();
  }
  // Draw cursor line
  cx.strokeStyle='#95d5b2';cx.lineWidth=2;
  cx.beginPath();cx.moveTo(cp,4);cx.lineTo(cp,h-4);cx.stroke();
  cx.fillStyle='#95d5b2';cx.beginPath();cx.arc(cp,4,3,0,Math.PI*2);cx.fill();
  anim=requestAnimationFrame(drawFrame);
}
function drawDone(){
  var w=cv.offsetWidth,h=cv.offsetHeight,mid=h/2;
  cx.clearRect(0,0,w,h);
  cx.strokeStyle='#e94560';cx.lineWidth=1.5;
  cx.beginPath();
  for(var i=0;i<w;i++){
    var y=getWaveY(i,w,h,mid);
    if(i===0)cx.moveTo(i,y);else cx.lineTo(i,y);
  }
  cx.stroke();
  var cp=w-1;
  cx.strokeStyle='#95d5b2';cx.lineWidth=2;
  cx.beginPath();cx.moveTo(cp,4);cx.lineTo(cp,h-4);cx.stroke();
  cx.fillStyle='#95d5b2';cx.beginPath();cx.arc(cp,4,3,0,Math.PI*2);cx.fill();
}
function startAnim(){
  playing=true;startTime=Date.now();estDuration=10;serverPct=0;
  genWave();
  drawFrame();
}
function stopAnim(){
  playing=false;
  if(anim){cancelAnimationFrame(anim);anim=null;}
  drawDone();
}
var currentLabel='Speak';
var savedGain=%GAIN%;
var savedRate=%RATE%;
var savedPitch=%PITCH%;
var savedVoice="%VOICE%";
var CASUAL_PRESETS={
  soft:{rate:1.13,pitch:0.4,gain:0.82},
  chatty:{rate:1.25,pitch:0.8,gain:0.88},
  sleepy:{rate:1.02,pitch:0.0,gain:0.78},
  cheerful:{rate:1.32,pitch:1.2,gain:0.90},
  neutral:{rate:1.19,pitch:0.5,gain:0.85},
  street:{rate:1.10,pitch:0.0,gain:0.80}
};

function applySavedSettings(){
  var vol=document.getElementById('vol');
  var rate=document.getElementById('rate');
  var pitch=document.getElementById('pitch');
  var voice=document.getElementById('voice');
  vol.value=savedGain;
  rate.value=savedRate;
  pitch.value=savedPitch;
  voice.value=savedVoice;
  document.getElementById('vv').textContent=parseFloat(savedGain).toFixed(1);
  document.getElementById('rv').textContent=parseFloat(savedRate).toFixed(2);
  document.getElementById('pv').textContent=parseFloat(savedPitch).toFixed(1);
}

function applyPreset(name){
  var p=CASUAL_PRESETS[name];
  if(!p){return;}

  var vol=document.getElementById('vol');
  var rate=document.getElementById('rate');
  var pitch=document.getElementById('pitch');

  vol.value=p.gain.toFixed(1);
  rate.value=p.rate.toFixed(2);
  pitch.value=p.pitch.toFixed(1);

  document.getElementById('vv').textContent=parseFloat(vol.value).toFixed(1);
  document.getElementById('rv').textContent=parseFloat(rate.value).toFixed(2);
  document.getElementById('pv').textContent=parseFloat(pitch.value).toFixed(1);

  setVolume(vol.value);
  ss('Preset applied: '+name,'info');
}

async function testTone(){
  var bt=document.getElementById('btnTone');
  bt.disabled=true;bt.textContent='Playing tone\u2026';
  ss('Playing 440 Hz test tone (2 sec)\u2026','info');
  try{
    var r=await fetch('/tone',{method:'POST',headers:{'Content-Type':'application/json'},body:'{}' });
    var j=await r.json();
    if(r.ok){ss('Tone played \u2714 If you heard it, I2S + PCM5102A are working.','ok');}
    else{ss('Tone error: '+(j.error||'Unknown'),'err');}
  }catch(e){ss('Connection error: '+e.message,'err');}
  finally{bt.disabled=false;bt.textContent='\u266B Test Tone (440 Hz \u2014 no internet needed)';}
}
function setVolume(val){
  fetch('/volume',{method:'POST',
    headers:{'Content-Type':'application/json'},
    body:JSON.stringify({gain:parseFloat(val)})
  }).catch(function(){});
}
function pollStatus(){
  if(!playing)return;
  fetch('/status').then(function(r){return r.json();}).then(function(j){
    if(!j.playing){
      stopAnim();
      ss('Done speaking \u2714','ok');
    }else{
      if(j.duration>0){
        estDuration=j.duration/1000;
        serverPct=Math.min(j.elapsed/j.duration,0.98);
      }
      setTimeout(pollStatus,300);
    }
  }).catch(function(){setTimeout(pollStatus,1000);});
}
async function speak(){
  var t=document.getElementById('txt').value.trim();
  if(!t){ss('Please enter some text','err');return;}
  var payload={
    text:t,
    rate:parseFloat(document.getElementById('rate').value),
    pitch:parseFloat(document.getElementById('pitch').value),
    voice:document.getElementById('voice').value
  };
  await sendRequest('/speak',payload,'Speak');
}
function setBusy(isBusy){
  var b=document.getElementById('btn');
  b.disabled=isBusy;
  if(isBusy){
    b.textContent='Processing\u2026';
  }else{
    b.textContent='Speak';
  }
}
async function sendRequest(path,payload,label){
  currentLabel=label;
  setBusy(true);
  ss('Sending speak request\u2026','info');
  try{
    var r=await fetch(path,{method:'POST',
      headers:{'Content-Type':'application/json'},
      body:JSON.stringify(payload)});
    var j=await r.json();
    if(r.ok){
      ss('Speaking\u2026','ok');
      startAnim();
      setTimeout(pollStatus,500);
    }else{ss('Error: '+(j.error||'Unknown'),'err');}
  }catch(e){ss('Connection error: '+e.message,'err');}
  finally{setBusy(false);}
}
function ss(m,c){var e=document.getElementById('st');e.textContent=m;e.className=c||'';}
window.onload=function(){initWf();applySavedSettings();};window.onresize=initWf;
</script>
</body>
</html>
)rawliteral";

// ============================================================
// Globals
// ============================================================
WebServer server(80);
Preferences prefs;

AudioFileSourcePROGMEM *srcMem   = nullptr;
AudioGeneratorMP3      *mp3      = nullptr;
AudioOutputI2S         *audioOut = nullptr;

uint8_t  *audioBuffer    = nullptr;    // decoded MP3 audio data
uint32_t  audioBufferLen = 0;

bool    pendingTTS   = false;
bool    audioPlaying = false;
String  pendingText;
float   pendingRate  = 1.0f;
float   pendingPitch = 0.0f;
String  pendingVoice = "en-US-Standard-F";
float   audioGain   = 1.0f;

unsigned long playbackStartMs    = 0;
unsigned long estimatedDurationMs = 0;

// LED / switch state
unsigned long ledLastToggleMs  = 0;
bool          ledState         = false;
bool          swLastRaw        = false;
bool          swDebounced      = false;
unsigned long swDebounceMs     = 0;
bool          swPressed        = false;
unsigned long swPressStartMs   = 0;
constexpr unsigned long SW_DEBOUNCE_MS   = 50;
constexpr unsigned long SW_LONG_PRESS_MS = 2000;

bool isValidVoiceId(const String &voice) {
  if (voice.length() == 0 || voice.length() > 40) return false;
  for (size_t i = 0; i < voice.length(); i++) {
    char c = voice[i];
    if (!((c >= 'a' && c <= 'z') ||
        (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') ||
        c == '-')) {
      return false;
    }
  }
  return true;
}

void loadPersistentSettings() {
  if (!prefs.begin("ttscfg", false)) {
    Serial.println("[cfg] NVS open failed; using defaults");
    return;
  }

  audioGain = constrain(prefs.getFloat("gain", audioGain), 0.0f, 4.0f);
  pendingRate = constrain(prefs.getFloat("rate", pendingRate), 0.25f, 4.0f);
  pendingPitch = constrain(prefs.getFloat("pitch", pendingPitch), -20.0f, 20.0f);
  String voice = prefs.getString("voice", pendingVoice);
  if (isValidVoiceId(voice)) {
    pendingVoice = voice;
  }

  Serial.printf("[cfg] Loaded gain=%.1f rate=%.1f pitch=%.1f voice=%s\n",
          audioGain, pendingRate, pendingPitch, pendingVoice.c_str());
}

void savePersistentSettings() {
  prefs.putFloat("gain", audioGain);
  prefs.putFloat("rate", pendingRate);
  prefs.putFloat("pitch", pendingPitch);
  prefs.putString("voice", pendingVoice);
}

// ============================================================
// Audio status callback — prints decoder messages to Serial
// ============================================================
void audioStatusCB(void *cbData, int code, const char *string) {
    (void)cbData;
    Serial.printf("[audio] status %d: %s\n", code, string);
}

// ============================================================
// Stop playback and free all audio resources
// ============================================================
void stopPlayback() {
    if (mp3)      { mp3->stop();      delete mp3;      mp3      = nullptr; }
    if (srcMem)   {                   delete srcMem;   srcMem   = nullptr; }
    if (audioOut) {                   delete audioOut; audioOut = nullptr; }
    if (audioBuffer) { free(audioBuffer); audioBuffer = nullptr; audioBufferLen = 0; }
    if (audioPlaying) {
        audioPlaying = false;
        Serial.println("[audio] Playback stopped, resources freed");
    }
}

// ============================================================
// Initiate TTS API call and begin streaming playback
// ============================================================
void startTTS() {
    stopPlayback();

    unsigned long ttsStartMs = millis();
    Serial.println("[tts] Connecting to Google Cloud TTS...");

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(20000);

    unsigned long connectStartMs = millis();
    if (!client.connect(TTS_HOST, TTS_PORT)) {
        Serial.println("[tts] Connection FAILED");
        return;
    }
    Serial.printf("[tts] Connected in %lu ms\n", millis() - connectStartMs);

    // --- Extract language code from voice name (e.g. "en-US-Standard-F" → "en-US") ---
    String langCode = "en-US";
    int secondDash = pendingVoice.indexOf('-', pendingVoice.indexOf('-') + 1);
    if (secondDash > 0) {
        langCode = pendingVoice.substring(0, secondDash);
    }

    // --- Build JSON request body ---
    DynamicJsonDocument doc(1024);
    doc["input"]["text"]                = pendingText;
    doc["voice"]["languageCode"]        = langCode;
    doc["voice"]["name"]                = pendingVoice;
    doc["audioConfig"]["audioEncoding"] = "MP3";
    doc["audioConfig"]["speakingRate"]  = pendingRate;
    doc["audioConfig"]["pitch"]         = pendingPitch;
    String jsonBody;
    serializeJson(doc, jsonBody);

    // --- Build API path with key ---
    String apiPath = "/v1/text:synthesize?key=";
    apiPath += TTS_API_KEY;

    // --- Send HTTP POST ---
    client.printf("POST %s HTTP/1.1\r\n", apiPath.c_str());
    client.printf("Host: %s\r\n", TTS_HOST);
    client.println("Content-Type: application/json");
    client.printf("Content-Length: %u\r\n", jsonBody.length());
    client.println("Connection: close");
    client.println();
    client.print(jsonBody);

    Serial.printf("[tts] POST sent (%u bytes body)\n", jsonBody.length());

    // --- Wait for response (feed watchdog so we don't reboot) ---
    unsigned long t0 = millis();
    while (!client.available() && client.connected() && (millis() - t0) < 20000) {
        esp_task_wdt_reset();
        delay(10);
    }
    if (!client.available()) {
        Serial.println("[tts] No response (timeout)");
        return;
    }
    Serial.printf("[tts] First response bytes after %lu ms\n", millis() - t0);

    // --- Read status line ---
    String statusLine = client.readStringUntil('\n');
    statusLine.trim();
    Serial.printf("[tts] Response: %s\n", statusLine.c_str());

    if (statusLine.indexOf("200") < 0) {
        Serial.println("[tts] API error:");
        while (client.available()) {
            Serial.println(client.readStringUntil('\n'));
        }
        return;
    }

    // --- Read response headers and detect body framing ---
    bool chunked = false;
    int contentLength = -1;
    while (client.connected()) {
      String header = client.readStringUntil('\n');
      header.trim();
      if (header.length() == 0) break;

      String h = header;
      h.toLowerCase();
      if (h.startsWith("transfer-encoding:") && h.indexOf("chunked") >= 0) {
        chunked = true;
      } else if (h.startsWith("content-length:")) {
        contentLength = h.substring(15).toInt();
      }
    }

    // --- Stream body, extract base64 without holding full response in RAM ---
    // Search for "audioContent" key with tolerant JSON parsing around ':' and quotes.
    const char *keyNeedle = "\"audioContent\"";
    const int keyNeedleLen = 14;
    int keyPos = 0;
    bool keyFound = false;
    bool waitForQuote = false;
    bool inAudio = false;
    String b64 = "";
    b64.reserve(80000);

    auto scanBodyChar = [&](char c) -> bool {
      if (!inAudio) {
        if (!keyFound) {
          if (c == keyNeedle[keyPos]) {
            keyPos++;
            if (keyPos == keyNeedleLen) {
              keyFound = true;
              waitForQuote = true;
              keyPos = 0;
            }
          } else {
            keyPos = (c == keyNeedle[0]) ? 1 : 0;
          }
          return false;
        }

        if (waitForQuote) {
          if (c == ':') {
            return false;
          }
          if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            return false;
          }
          if (c == '"') {
            inAudio = true;
            return false;
          }

          // Unexpected token after key: restart search to avoid false lockup.
          keyFound = false;
          waitForQuote = false;
          keyPos = (c == keyNeedle[0]) ? 1 : 0;
        }
        return false;
      }

      if (c == '"') {
        return true;  // end of base64 value
      }
      b64 += c;
      return false;
    };

    bool gotAudio = false;
    unsigned long bodyT0 = millis();
    if (chunked) {
      while ((millis() - bodyT0) < 30000) {
        esp_task_wdt_reset();

        String sizeLine = client.readStringUntil('\n');
        sizeLine.trim();
        if (sizeLine.length() == 0) {
          continue;
        }

        int semi = sizeLine.indexOf(';');
        if (semi >= 0) {
          sizeLine = sizeLine.substring(0, semi);
        }
        sizeLine.trim();
        int chunkSize = (int)strtol(sizeLine.c_str(), nullptr, 16);
        if (chunkSize <= 0) {
          break;
        }

        for (int i = 0; i < chunkSize; i++) {
          unsigned long waitT0 = millis();
          while (!client.available() && (millis() - waitT0) < 2000) {
            esp_task_wdt_reset();
            delay(1);
          }
          if (!client.available()) break;
          char c = (char)client.read();
          if (scanBodyChar(c)) {
            gotAudio = true;
            break;
          }
        }

        // consume trailing CRLF after each chunk
        if (client.available()) client.read();
        if (client.available()) client.read();

        if (gotAudio) break;
      }
    } else {
      int readCount = 0;
      while ((client.connected() || client.available()) && (millis() - bodyT0) < 30000) {
        esp_task_wdt_reset();
        if (!client.available()) { delay(1); continue; }

        char c = (char)client.read();
        readCount++;
        if (scanBodyChar(c)) {
          gotAudio = true;
          break;
        }

        if (contentLength > 0 && readCount >= contentLength) {
          break;
        }
      }
    }

    if (!gotAudio && inAudio && b64.length() > 0) {
      gotAudio = true;
    }
    Serial.printf("[tts] Base64 audio: %u chars (body parse %lu ms)\n", b64.length(), millis() - bodyT0);
    if (b64.length() == 0) {
        Serial.println("[tts] No audioContent found in response");
        return;
    }

    // --- Decode base64 to binary MP3 ---
    size_t decodedLen = 0;
    // First call: get required output size
    mbedtls_base64_decode(nullptr, 0, &decodedLen,
                          (const unsigned char *)b64.c_str(), b64.length());

    Serial.printf("[tts] Free heap before alloc: %u bytes, need %u\n",
                  ESP.getFreeHeap(), (unsigned)decodedLen);

    if (ESP.getFreeHeap() < decodedLen + 20000) {
        Serial.println("[tts] Insufficient heap — cannot decode audio");
        return;
    }

    audioBuffer = (uint8_t *)malloc(decodedLen);
    if (!audioBuffer) {
        Serial.printf("[tts] Failed to allocate %u bytes for audio\n", decodedLen);
        return;
    }

    unsigned long decodeStartMs = millis();
    int ret = mbedtls_base64_decode(audioBuffer, decodedLen, &audioBufferLen,
                                     (const unsigned char *)b64.c_str(), b64.length());
    b64 = "";  // free base64 string memory
    if (ret != 0) {
        Serial.printf("[tts] Base64 decode failed: %d\n", ret);
        free(audioBuffer);
        audioBuffer = nullptr;
        return;
    }
    Serial.printf("[tts] Decoded MP3: %u bytes in %lu ms\n", audioBufferLen, millis() - decodeStartMs);

    // --- Build audio pipeline and play ---
    srcMem = new AudioFileSourcePROGMEM(audioBuffer, audioBufferLen);

    // 32 DMA buffers (~93 ms at 44.1 kHz stereo 16-bit) prevents underruns
    // when server.handleClient() briefly blocks the loop.
    audioOut = new AudioOutputI2S(0, AudioOutputI2S::EXTERNAL_I2S, 32);
    audioOut->SetPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
    audioOut->SetGain(audioGain);

    mp3 = new AudioGeneratorMP3();
    mp3->RegisterStatusCB(audioStatusCB, nullptr);

    if (mp3->begin(srcMem, audioOut)) {
        audioPlaying = true;
        playbackStartMs = millis();
        // Estimate duration: Google TTS MP3 is typically ~32kbps
        estimatedDurationMs = (unsigned long)(audioBufferLen * 8UL / 32);
        Serial.printf("[audio] Playback started, est. %lu ms (total TTS pipeline %lu ms)\n", estimatedDurationMs, playbackStartMs - ttsStartMs);
    } else {
        Serial.println("[audio] Failed to start MP3 decoder");
        stopPlayback();
    }
}

// ============================================================
// Web server handlers
// ============================================================
void handleRoot() {
    String page = FPSTR(HTML_PAGE);
    page.replace("%FW_VER%", FIRMWARE_VERSION);
    page.replace("%BUILD_DATE%", __DATE__);
    page.replace("%BUILD_TIME%", __TIME__);
    page.replace("%GAIN%", String(audioGain, 1));
    page.replace("%RATE%", String(pendingRate, 2));
    page.replace("%PITCH%", String(pendingPitch, 1));
    page.replace("%VOICE%", pendingVoice);
    server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    server.sendHeader("Pragma", "no-cache");
    server.sendHeader("Expires", "0");
    server.send(200, "text/html", page);
}

bool applyTTSRequestSettings(const JsonDocument &doc, bool requireText) {
  if (requireText) {
    pendingText = doc["text"] | "";
    if (pendingText.length() == 0) {
      return false;
    }
  }

  pendingRate  = doc["rate"]  | pendingRate;
  pendingPitch = doc["pitch"] | pendingPitch;
  String voice = doc["voice"] | pendingVoice;
  if (isValidVoiceId(voice)) {
    pendingVoice = voice;
  }

  // Clamp parameters to API limits
  pendingRate  = constrain(pendingRate,  0.25f, 4.0f);
  pendingPitch = constrain(pendingPitch, -20.0f, 20.0f);
  return true;
}

void handleSpeak() {
    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"error\":\"No request body\"}");
        return;
    }

    DynamicJsonDocument doc(2048);
    DeserializationError err = deserializeJson(doc, server.arg("plain"));
    if (err) {
        Serial.printf("[web] JSON parse error: %s\n", err.c_str());
        server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
    }

    if (!applyTTSRequestSettings(doc, true)) {
        server.send(400, "application/json", "{\"error\":\"Empty text\"}");
        return;
    }

    savePersistentSettings();

    pendingTTS = true;
    server.send(200, "application/json", "{\"status\":\"ok\"}");

    Serial.printf("[web] TTS request: \"%s\" rate=%.1f pitch=%.1f voice=%s\n",
                  pendingText.c_str(), pendingRate, pendingPitch, pendingVoice.c_str());
}

void handleVolume() {
    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"error\":\"No body\"}");
        return;
    }
    DynamicJsonDocument doc(256);
    DeserializationError err = deserializeJson(doc, server.arg("plain"));
    if (err) {
        server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
    }
    float g = doc["gain"] | audioGain;
    audioGain = constrain(g, 0.0f, 4.0f);
    if (audioOut) audioOut->SetGain(audioGain);
    savePersistentSettings();
    server.send(200, "application/json", "{\"status\":\"ok\"}");
    Serial.printf("[web] Volume set to %.1f\n", audioGain);
}

void playTestTone() {
    stopPlayback();  // release ESP8266Audio I2S driver first

    const i2s_port_t PORT = I2S_NUM_0;
    i2s_config_t cfg = {};
    cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    cfg.sample_rate = 44100;
    cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags = 0;
    cfg.dma_buf_count = 8;
    cfg.dma_buf_len = 64;
    cfg.use_apll = false;
    cfg.tx_desc_auto_clear = true;

    if (i2s_driver_install(PORT, &cfg, 0, NULL) != ESP_OK) {
        Serial.println("[tone] I2S driver install failed");
        return;
    }
    i2s_pin_config_t pins = {};
    pins.bck_io_num   = I2S_BCLK;
    pins.ws_io_num    = I2S_LRC;
    pins.data_out_num = I2S_DOUT;
    pins.data_in_num  = I2S_PIN_NO_CHANGE;
    if (i2s_set_pin(PORT, &pins) != ESP_OK) {
        Serial.println("[tone] I2S set pin failed");
        i2s_driver_uninstall(PORT);
        return;
    }

    Serial.println("[tone] Playing 440 Hz test tone for 2 seconds...");
    const int RATE      = 44100;
    const float FREQ    = 440.0f;
    const int TOTAL     = RATE * 2;  // 2 seconds
    const int16_t AMP   = 20000;
    const int BATCH     = 64;
    int16_t buf[BATCH * 2];
    size_t written;
    for (int s = 0; s < TOTAL; s += BATCH) {
        int n = min(BATCH, TOTAL - s);
        for (int i = 0; i < n; i++) {
            int16_t v = (int16_t)(AMP * sinf(2.0f * (float)M_PI * FREQ * (s + i) / RATE));
            buf[i * 2]     = v;
            buf[i * 2 + 1] = v;
        }
        i2s_write(PORT, buf, n * 4, &written, portMAX_DELAY);
        esp_task_wdt_reset();
    }
    i2s_zero_dma_buffer(PORT);
    delay(100);
    i2s_driver_uninstall(PORT);
    Serial.println("[tone] Done");
}

void handleTone() {
    server.send(200, "application/json", "{\"status\":\"ok\"}");
    playTestTone();
}

void handleStatus() {
    bool isActive = audioPlaying || pendingTTS;
    unsigned long elapsed = 0;
    unsigned long duration = estimatedDurationMs;
    if (audioPlaying && playbackStartMs > 0) {
        elapsed = millis() - playbackStartMs;
    }
    String json = "{\"playing\":";
    json += isActive ? "true" : "false";
    json += ",\"elapsed\":";
    json += String(elapsed);
    json += ",\"duration\":";
    json += String(duration);
    json += "}";
    server.send(200, "application/json", json);
}

void handleNotFound() {
    server.send(404, "text/plain", "Not found");
}

// ============================================================
// Setup
// ============================================================
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n========================================");
    Serial.printf("  ESP32 Text-to-Speech Device  v%s\n", FIRMWARE_VERSION);
    Serial.println("========================================");

    // --- LED + Switch setup ---
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);
    pinMode(SWITCH_PIN, INPUT);  // pin 35: input-only, no internal pull-up

    // --- Connect to WiFi ---
    Serial.printf("[wifi] Connecting to %s", WIFI_SSID);
    WiFi.mode(WIFI_STA);

    if (USE_STATIC_IP) {
      if (WiFi.config(STATIC_IP, STATIC_GW, STATIC_MASK, STATIC_DNS1, STATIC_DNS2)) {
        Serial.printf("\n[wifi] Static IP configured: %s\n", STATIC_IP.toString().c_str());
      } else {
        Serial.println("\n[wifi] Static IP config failed; continuing with DHCP");
      }
    }

    WiFi.setHostname(DEVICE_HOSTNAME);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    unsigned long wifiTimeout = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - wifiTimeout > 20000) {
            Serial.println("\n[wifi] Connection FAILED — restarting...");
            ESP.restart();
        }
        // Slow blink while connecting
        ledState = !ledState;
        digitalWrite(LED_PIN, ledState);
        delay(500);
        Serial.print(".");
    }
    Serial.println();
    Serial.printf("[wifi] Connected!  IP: %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("[wifi] Hostname: %s\n", DEVICE_HOSTNAME);
    WiFi.setSleep(false);
    Serial.println("[wifi] Power save disabled for lower request latency");

    if (MDNS.begin(DEVICE_HOSTNAME)) {
      Serial.printf("[mdns] Ready: http://%s.local\n", DEVICE_HOSTNAME);
    } else {
      Serial.println("[mdns] Start failed (some networks block mDNS)");
    }

    loadPersistentSettings();
    digitalWrite(LED_PIN, HIGH);   // solid ON = ready
    ledState = true;

    // --- Start web server ---
    server.on("/",       HTTP_GET,  handleRoot);
    server.on("/speak",  HTTP_POST, handleSpeak);
    server.on("/volume", HTTP_POST, handleVolume);
    server.on("/tone",   HTTP_POST, handleTone);
    server.on("/status", HTTP_GET,  handleStatus);
    server.onNotFound(handleNotFound);
    server.begin();
    Serial.println("[web]  HTTP server started on port 80");
    Serial.printf("[web]  Open http://%s in your browser\n", WiFi.localIP().toString().c_str());
    Serial.printf("[web]  Or try http://%s.local\n", DEVICE_HOSTNAME);
}

// ============================================================
// Main loop
// ============================================================
void loop() {
    // Service audio first so DMA buffers stay fed before handling network requests.
    if (mp3 && mp3->isRunning()) {
        if (!mp3->loop()) {
            Serial.println("[audio] Playback finished");
            stopPlayback();
        }
    }
    // Start a new TTS request if one is pending and nothing is playing
    else if (pendingTTS) {
        pendingTTS = false;
        startTTS();
    }

    server.handleClient();

    unsigned long now = millis();

    // --- LED update ---
    bool busy = audioPlaying || pendingTTS;
    if (swPressed && swDebounced && (now - swPressStartMs) >= SW_LONG_PRESS_MS) {
        // Rapid blink warning: long-press reset imminent
        if (now - ledLastToggleMs >= 50) {
            ledLastToggleMs = now;
            ledState = !ledState;
            digitalWrite(LED_PIN, ledState);
        }
    } else if (busy) {
        // Fast blink while speaking / processing
        if (now - ledLastToggleMs >= 150) {
            ledLastToggleMs = now;
            ledState = !ledState;
            digitalWrite(LED_PIN, ledState);
        }
    } else {
        // Solid ON when idle
        if (!ledState) {
            ledState = true;
            digitalWrite(LED_PIN, HIGH);
        }
    }

    // --- Switch debounce and press detection ---
    // Pin 35 is pulled HIGH to 3.3V; switch connects to GND, so LOW = pressed.
    bool raw = (digitalRead(SWITCH_PIN) == LOW);
    if (raw != swLastRaw) {
        swDebounceMs = now;
        swLastRaw = raw;
    }
    if ((now - swDebounceMs) >= SW_DEBOUNCE_MS && swDebounced != raw) {
        swDebounced = raw;
        if (swDebounced) {
            // Falling edge: button pressed
            swPressed = true;
            swPressStartMs = now;
            Serial.println("[sw] Press started");
        } else if (swPressed) {
            // Rising edge: button released
            swPressed = false;
            unsigned long held = now - swPressStartMs;
            if (held >= SW_LONG_PRESS_MS) {
                Serial.println("[sw] Long press — hardware reset");
                digitalWrite(LED_PIN, LOW);
                delay(100);
                ESP.restart();
            } else {
                Serial.println("[sw] Short press — speaking system ready");
                if (!pendingTTS && !audioPlaying) {
                    pendingText  = "System ready";
                    pendingRate  = 1.0f;
                    pendingPitch = 0.0f;
                    pendingVoice = "en-US-Standard-F";
                    pendingTTS   = true;
                }
            }
        }
    }
}
