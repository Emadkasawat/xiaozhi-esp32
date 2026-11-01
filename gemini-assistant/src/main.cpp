// Standalone Gemini Live voice assistant.
//
// The device talks straight to Google's Gemini Live API over a secure
// WebSocket: no xiaozhi server and no server of our own. Hold the talk
// button (GPIO47) or BOOT (GPIO0) to speak, release to get the answer.
//
// Wiring (same as xiaozhi "bread-compact-wifi"):
//   INMP441 mic : WS=4  SCK=5  SD=6  (L/R -> GND, VDD -> 3V3)
//   MAX98357A   : DIN=7 BCLK=15 LRC=16
//   SH1106 OLED : SDA=41 SCL=42
//   Talk button : GPIO47 to GND (BOOT on GPIO0 works too)
//
// Wi-Fi and the Gemini API key are entered on the device itself through the
// "Gemini-Setup" Wi-Fi hotspot, and stored in flash (never in this repo).

#include <Arduino.h>
#include <Preferences.h>
#include <U8g2lib.h>
#include <WebSocketsClient.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <Wire.h>
#include <driver/i2s.h>
#include <freertos/stream_buffer.h>
#include <mbedtls/base64.h>

#include "certs.h"

// ---------------------------------------------------------------- pins
static const int PIN_MIC_WS = 4;
static const int PIN_MIC_SCK = 5;
static const int PIN_MIC_SD = 6;
static const int PIN_SPK_DIN = 7;
static const int PIN_SPK_BCLK = 15;
static const int PIN_SPK_LRC = 16;
static const int PIN_OLED_SDA = 41;
static const int PIN_OLED_SCL = 42;
static const int PIN_TALK = 47;
static const int PIN_BOOT = 0;

static const i2s_port_t I2S_MIC = I2S_NUM_0;
static const i2s_port_t I2S_SPK = I2S_NUM_1;

static const int MIC_RATE = 16000;   // Gemini input: 16 kHz, 16-bit, mono
static const int SPK_RATE = 24000;   // Gemini output: 24 kHz, 16-bit, mono
static const int MIC_CHUNK_SAMPLES = 1600;  // 100 ms per message

static const char *GEMINI_HOST = "generativelanguage.googleapis.com";
static const char *GEMINI_PATH =
    "/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent?key=";
static const char *DEFAULT_MODEL = "models/gemini-3.8-live";

static const char *SYSTEM_PROMPT =
    "أنت مساعد صوتي ودود اسمه جيميناي، شغال جوه جهاز صغير. "
    "رد دايمًا بالعربي المصري، بجمل قصيرة وواضحة تتقال بصوت عالي، "
    "من غير رموز أو قوائم أو تنسيق. لو المستخدم كلمك بلغة تانية رد بنفس لغته. "
    "لو السؤال محتاج معلومة حديثة استخدم البحث.";

// ---------------------------------------------------------------- settings
struct Settings {
  String apiKey;
  String model;
  int volume = 70;   // 1..100
  bool flip = false; // rotate the screen 180 degrees
  bool handsFree = true;  // always listening (no button needed)
  int micThreshold = 400; // hands-free: mic level that counts as speech
} settings;

Preferences prefs;

void loadSettings() {
  prefs.begin("gemini", true);
  settings.apiKey = prefs.getString("key", "");
  settings.model = prefs.getString("model", DEFAULT_MODEL);
  settings.volume = prefs.getInt("volume", 70);
  settings.flip = prefs.getBool("flip", false);
  settings.handsFree = prefs.getBool("handsfree", true);
  settings.micThreshold = prefs.getInt("micthr", 400);
  prefs.end();
  if (settings.model.isEmpty()) settings.model = DEFAULT_MODEL;
  settings.volume = constrain(settings.volume, 1, 100);
}

void saveSettings() {
  prefs.begin("gemini", false);
  prefs.putString("key", settings.apiKey);
  prefs.putString("model", settings.model);
  prefs.putInt("volume", settings.volume);
  prefs.putBool("flip", settings.flip);
  prefs.putBool("handsfree", settings.handsFree);
  prefs.putInt("micthr", settings.micThreshold);
  prefs.end();
}

// ---------------------------------------------------------------- display
U8G2_SH1106_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE);

enum class State { Boot, Setup, Wifi, Connecting, Ready, Listening, Thinking, Speaking, Error };
volatile State state = State::Boot;
String detail;
bool screenDirty = true;

const char *stateTitle(State s) {
  switch (s) {
    case State::Boot: return "Starting";
    case State::Setup: return "Setup mode";
    case State::Wifi: return "Wi-Fi...";
    case State::Connecting: return "Gemini...";
    case State::Ready: return "Ready";
    case State::Listening: return "Listening";
    case State::Thinking: return "Thinking";
    case State::Speaking: return "Speaking";
    case State::Error: return "Error";
  }
  return "";
}

void setState(State s, const String &d = "") {
  if (s != state || d != detail) {
    state = s;
    detail = d;
    screenDirty = true;
    Serial.printf("[state] %s %s\n", stateTitle(s), d.c_str());
  }
}

// Word-wraps text into lines of at most `width` characters.
void drawWrapped(const String &text, int y, int width, int lineHeight, int maxLines) {
  int start = 0;
  int line = 0;
  while (start < (int)text.length() && line < maxLines) {
    int end = min((int)text.length(), start + width);
    if (end < (int)text.length()) {
      int space = text.lastIndexOf(' ', end);
      if (space > start) end = space;
    }
    oled.drawStr(0, y + line * lineHeight, text.substring(start, end).c_str());
    start = end;
    while (start < (int)text.length() && text[start] == ' ') start++;
    line++;
  }
}

void drawScreen() {
  if (!screenDirty) return;
  screenDirty = false;
  oled.clearBuffer();
  oled.setFont(u8g2_font_helvB14_tr);
  oled.drawStr(0, 16, stateTitle(state));
  oled.drawHLine(0, 20, 128);
  oled.setFont(u8g2_font_6x10_tr);
  String d = detail;
  if (d.isEmpty()) {
    switch (state) {
      case State::Ready: d = settings.handsFree ? "Just talk" : "Hold the button and talk"; break;
      case State::Listening: d = settings.handsFree ? "" : "Release when done"; break;
      case State::Thinking: d = "Waiting for Gemini"; break;
      case State::Speaking: d = "Press to interrupt"; break;
      default: break;
    }
  }
  drawWrapped(d, 32, 21, 11, 4);
  oled.sendBuffer();
}

// ---------------------------------------------------------------- audio
// Playback: decoded 24 kHz PCM goes into a big PSRAM stream buffer and a
// dedicated task feeds it to the amplifier.
static const size_t PLAY_BUF_BYTES = 2 * 1024 * 1024;  // ~43 s of audio
StreamBufferHandle_t playBuf = nullptr;
StaticStreamBuffer_t playBufStruct;
volatile bool flushPlayback = false;
volatile bool playbackActive = false;

void setupI2S() {
  i2s_config_t mic = {};
  mic.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  mic.sample_rate = MIC_RATE;
  mic.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  mic.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;  // pick the live channel in software
  mic.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  mic.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  mic.dma_buf_count = 8;
  mic.dma_buf_len = 256;
  mic.use_apll = false;
  i2s_driver_install(I2S_MIC, &mic, 0, nullptr);
  i2s_pin_config_t micPins = {};
  micPins.mck_io_num = I2S_PIN_NO_CHANGE;
  micPins.bck_io_num = PIN_MIC_SCK;
  micPins.ws_io_num = PIN_MIC_WS;
  micPins.data_out_num = I2S_PIN_NO_CHANGE;
  micPins.data_in_num = PIN_MIC_SD;
  i2s_set_pin(I2S_MIC, &micPins);
  i2s_stop(I2S_MIC);

  i2s_config_t spk = {};
  spk.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  spk.sample_rate = SPK_RATE;
  spk.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  spk.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  spk.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  spk.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  spk.dma_buf_count = 8;
  spk.dma_buf_len = 256;
  spk.use_apll = false;
  spk.tx_desc_auto_clear = true;  // silence instead of buzzing on underrun
  i2s_driver_install(I2S_SPK, &spk, 0, nullptr);
  i2s_pin_config_t spkPins = {};
  spkPins.mck_io_num = I2S_PIN_NO_CHANGE;
  spkPins.bck_io_num = PIN_SPK_BCLK;
  spkPins.ws_io_num = PIN_SPK_LRC;
  spkPins.data_out_num = PIN_SPK_DIN;
  spkPins.data_in_num = I2S_PIN_NO_CHANGE;
  i2s_set_pin(I2S_SPK, &spkPins);
  i2s_zero_dma_buffer(I2S_SPK);
}

void playbackTask(void *) {
  static int16_t mono[512];
  static int16_t stereo[1024];
  for (;;) {
    if (flushPlayback) {
      while (xStreamBufferReceive(playBuf, mono, sizeof(mono), 0) > 0) {
      }
      i2s_zero_dma_buffer(I2S_SPK);
      flushPlayback = false;
    }
    size_t got = xStreamBufferReceive(playBuf, mono, sizeof(mono), pdMS_TO_TICKS(40));
    if (got == 0) {
      playbackActive = false;
      continue;
    }
    playbackActive = true;
    size_t n = got / 2;
    int gain = settings.volume * 256 / 100;
    for (size_t i = 0; i < n; i++) {
      int32_t v = ((int32_t)mono[i] * gain) >> 8;
      stereo[2 * i] = stereo[2 * i + 1] = (int16_t)v;
    }
    size_t written = 0;
    i2s_write(I2S_SPK, stereo, n * 4, &written, portMAX_DELAY);
  }
}

// Queues PCM for playback, waiting for room if the buffer is full.
void queuePlayback(const uint8_t *data, size_t len) {
  len &= ~(size_t)1;
  size_t sent = 0;
  while (sent < len && !flushPlayback) {
    sent += xStreamBufferSend(playBuf, data + sent, len - sent, pdMS_TO_TICKS(200));
  }
}

bool playbackIdle() {
  return !playbackActive && xStreamBufferIsEmpty(playBuf) == pdTRUE;
}

// ---------------------------------------------------------------- Gemini
extern bool recording;
extern bool gateOpen;
void closeGate();

WebSocketsClient ws;
String wsPath;
bool sessionReady = false;
bool waitingForReply = false;
bool turnDone = true;
String closeReason;

uint8_t *decodeBuf = nullptr;
size_t decodeBufSize = 0;
uint8_t *fragBuf = nullptr;
size_t fragLen = 0;
size_t fragCap = 0;

// Called by the (locally patched) WebSockets library when the server closes.
void websocketsOnClose(uint16_t code, const char *reason, size_t len) {
  closeReason = String(code) + " ";
  for (size_t i = 0; i < len && i < 120; i++) closeReason += reason[i];
  Serial.printf("[ws] closed by server: %s\n", closeReason.c_str());
}

void sendSetup() {
  String msg;
  msg.reserve(1500);
  msg += "{\"setup\":{\"model\":\"";
  msg += settings.model;
  msg += "\",\"generationConfig\":{\"responseModalities\":[\"AUDIO\"],"
         "\"speechConfig\":{\"voiceConfig\":{\"prebuiltVoiceConfig\":{\"voiceName\":\"Kore\"}}}},"
         "\"systemInstruction\":{\"parts\":[{\"text\":\"";
  msg += SYSTEM_PROMPT;
  msg += "\"}]},"
         "\"tools\":[{\"googleSearch\":{}}]";
  if (!settings.handsFree) {
    // Push-to-talk: we mark the start and end of speech ourselves.
    msg += ",\"realtimeInputConfig\":{\"automaticActivityDetection\":{\"disabled\":true}}";
  }
  msg += "}}";
  ws.sendTXT(msg);
}

void sendRealtime(const char *body) {
  String msg = "{\"realtimeInput\":{";
  msg += body;
  msg += "}}";
  ws.sendTXT(msg);
}

bool ensureDecodeBuf(size_t need) {
  if (need <= decodeBufSize) return true;
  free(decodeBuf);
  decodeBuf = (uint8_t *)ps_malloc(need);
  decodeBufSize = decodeBuf ? need : 0;
  return decodeBuf != nullptr;
}

void handleServerMessage(char *msg, size_t len) {
  msg[len] = 0;  // payload buffers always have one spare byte

  if (strstr(msg, "\"setupComplete\"")) {
    sessionReady = true;
    closeReason = "";
    setState(State::Ready);
  }

  // Audio chunks: serverContent.modelTurn.parts[].inlineData.data (base64 PCM).
  char *p = msg;
  while ((p = strstr(p, "\"data\"")) != nullptr) {
    p += 6;
    while (*p == ' ' || *p == ':' || *p == '\n' || *p == '\r' || *p == '\t') p++;
    if (*p != '"') continue;
    char *start = p + 1;
    char *end = strchr(start, '"');
    if (!end) break;
    size_t b64len = end - start;
    p = end + 1;
    if (recording) continue;  // user is holding the button; drop old speech
    if (gateOpen) closeGate();  // Gemini decided we finished talking
    if (!ensureDecodeBuf(b64len * 3 / 4 + 4)) continue;
    size_t outLen = 0;
    if (mbedtls_base64_decode(decodeBuf, decodeBufSize, &outLen, (const unsigned char *)start, b64len) == 0 &&
        outLen > 0) {
      setState(State::Speaking);
      waitingForReply = false;
      queuePlayback(decodeBuf, outLen);
    }
  }

  if (strstr(msg, "\"interrupted\"")) {
    flushPlayback = true;
  }
  if (strstr(msg, "\"turnComplete\"")) {
    turnDone = true;
  }
  if (strstr(msg, "\"error\"")) {
    Serial.printf("[gemini] %.300s\n", msg);
  }
}

void appendFragment(const uint8_t *data, size_t len) {
  if (fragLen + len + 1 > fragCap) {
    size_t cap = max(fragCap * 2, fragLen + len + 1);
    uint8_t *n = (uint8_t *)ps_realloc(fragBuf, cap);
    if (!n) return;
    fragBuf = n;
    fragCap = cap;
  }
  memcpy(fragBuf + fragLen, data, len);
  fragLen += len;
}

void wsEvent(WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      Serial.println("[ws] connected");
      setState(State::Connecting, "Starting session");
      sessionReady = false;
      sendSetup();
      break;
    case WStype_DISCONNECTED:
      Serial.println("[ws] disconnected");
      sessionReady = false;
      if (!closeReason.isEmpty()) {
        setState(State::Error, closeReason);
      } else if (state != State::Error) {
        setState(State::Connecting, "Reconnecting");
      }
      break;
    case WStype_TEXT:
    case WStype_BIN:
      if (payload && length) handleServerMessage((char *)payload, length);
      break;
    case WStype_FRAGMENT_TEXT_START:
    case WStype_FRAGMENT_BIN_START:
      fragLen = 0;
      appendFragment(payload, length);
      break;
    case WStype_FRAGMENT:
      appendFragment(payload, length);
      break;
    case WStype_FRAGMENT_FIN:
      appendFragment(payload, length);
      if (fragBuf && fragLen) handleServerMessage((char *)fragBuf, fragLen);
      fragLen = 0;
      break;
    default:
      break;
  }
}

void connectGemini() {
  wsPath = String(GEMINI_PATH) + settings.apiKey;
  ws.beginSslWithCA(GEMINI_HOST, 443, wsPath.c_str(), GOOGLE_ROOT_CA, "");
  ws.onEvent(wsEvent);
  ws.setReconnectInterval(4000);
  setState(State::Connecting);
}

// ---------------------------------------------------------------- mic
bool recording = false;       // push-to-talk: button held
int16_t micChunk[MIC_CHUNK_SAMPLES];
size_t micFill = 0;
char *micJson = nullptr;
size_t micJsonSize = 0;

// Hands-free gate: audio is only streamed while someone is speaking, so the
// API is not billed for hours of room silence and the speaker is not heard.
static const int PREROLL_CHUNKS = 3;          // 300 ms kept from before speech
static const uint32_t HANG_MS = 1500;         // keep streaming after speech stops
static const uint32_t ECHO_GUARD_MS = 600;    // ignore mic right after playback
int16_t preroll[PREROLL_CHUNKS][MIC_CHUNK_SAMPLES];
int prerollCount = 0;
int prerollHead = 0;
bool gateOpen = false;
uint32_t lastLoudMs = 0;
uint32_t lastPlaybackMs = 0;
int micLevel = 0;

void sendPcm(const int16_t *samples, size_t count) {
  if (count == 0) return;
  size_t bytes = count * 2;
  size_t b64 = 0;
  const char *head = "{\"realtimeInput\":{\"audio\":{\"mimeType\":\"audio/pcm;rate=16000\",\"data\":\"";
  const char *tail = "\"}}}";
  size_t headLen = strlen(head);
  size_t need = headLen + (bytes + 2) / 3 * 4 + strlen(tail) + 2;
  if (need > micJsonSize) {
    free(micJson);
    micJson = (char *)ps_malloc(need);
    micJsonSize = micJson ? need : 0;
    if (!micJson) return;
  }
  memcpy(micJson, head, headLen);
  mbedtls_base64_encode((unsigned char *)micJson + headLen, micJsonSize - headLen, &b64,
                        (const unsigned char *)samples, bytes);
  strcpy(micJson + headLen + b64, tail);
  ws.sendTXT(micJson, headLen + b64 + strlen(tail));
}

uint32_t thinkingSinceMs = 0;

void closeGate() {
  if (!gateOpen) return;
  gateOpen = false;
  thinkingSinceMs = millis();
  sendRealtime("\"audioStreamEnd\":true");
  waitingForReply = true;
  turnDone = false;
  setState(State::Thinking);
}

// Hands-free: decide what to do with one finished 100 ms chunk.
void handsFreeChunk() {
  int64_t sum = 0;
  for (size_t i = 0; i < MIC_CHUNK_SAMPLES; i++) sum += (int32_t)micChunk[i] * micChunk[i];
  micLevel = (int)sqrt((double)sum / MIC_CHUNK_SAMPLES);
  uint32_t now = millis();

  bool speakerBusy = !playbackIdle() || now - lastPlaybackMs < ECHO_GUARD_MS;
  if (speakerBusy || !sessionReady) {
    // Half duplex: never send our own speaker sound back to Gemini.
    prerollCount = 0;
    if (gateOpen && !sessionReady) gateOpen = false;
    return;
  }

  bool loud = micLevel > settings.micThreshold;
  if (loud) {
    if (!gateOpen) {
      gateOpen = true;
      for (int i = 0; i < prerollCount; i++) {
        int idx = (prerollHead - prerollCount + i + PREROLL_CHUNKS) % PREROLL_CHUNKS;
        sendPcm(preroll[idx], MIC_CHUNK_SAMPLES);
      }
      prerollCount = 0;
      setState(State::Listening);
    }
    lastLoudMs = now;
    sendPcm(micChunk, MIC_CHUNK_SAMPLES);
  } else if (gateOpen) {
    sendPcm(micChunk, MIC_CHUNK_SAMPLES);
    if (now - lastLoudMs > HANG_MS) closeGate();
  } else {
    memcpy(preroll[prerollHead], micChunk, sizeof(micChunk));
    prerollHead = (prerollHead + 1) % PREROLL_CHUNKS;
    if (prerollCount < PREROLL_CHUNKS) prerollCount++;
  }
}

void onMicChunk() {
  if (settings.handsFree) {
    handsFreeChunk();
  } else {
    sendPcm(micChunk, micFill);
  }
  micFill = 0;
}

void readMic() {
  static int32_t raw[512];  // 256 stereo frames
  size_t got = 0;
  if (i2s_read(I2S_MIC, raw, sizeof(raw), &got, pdMS_TO_TICKS(10)) != ESP_OK || got == 0) return;
  size_t frames = got / 8;
  // The INMP441 drives only one slot; use whichever slot carries signal.
  int64_t e0 = 0, e1 = 0;
  for (size_t i = 0; i < frames; i++) {
    e0 += llabs((int64_t)(raw[2 * i] >> 14));
    e1 += llabs((int64_t)(raw[2 * i + 1] >> 14));
  }
  int slot = e1 > e0 ? 1 : 0;
  for (size_t i = 0; i < frames; i++) {
    int32_t s = raw[2 * i + slot] >> 14;
    if (s > 32767) s = 32767;
    if (s < -32768) s = -32768;
    micChunk[micFill++] = (int16_t)s;
    if (micFill == MIC_CHUNK_SAMPLES) onMicChunk();
  }
}

void startTalking() {
  flushPlayback = true;  // barge-in: stop whatever Gemini is saying
  micFill = 0;
  i2s_start(I2S_MIC);
  sendRealtime("\"activityStart\":{}");
  recording = true;
  turnDone = false;
  setState(State::Listening);
}

void stopTalking() {
  recording = false;
  readMic();
  sendPcm(micChunk, micFill);
  micFill = 0;
  i2s_stop(I2S_MIC);
  sendRealtime("\"activityEnd\":{}");
  waitingForReply = true;
  setState(State::Thinking);
}

// ---------------------------------------------------------------- setup portal
bool talkPressed() {
  return digitalRead(PIN_TALK) == LOW || digitalRead(PIN_BOOT) == LOW;
}

void runSetupPortal(bool forced) {
  WiFiManager wm;
  WiFiManagerParameter keyParam("key", "Gemini API key (leave empty to keep current)", "", 80);
  WiFiManagerParameter modelParam("model", "Model", settings.model.c_str(), 60);
  char volStr[8];
  snprintf(volStr, sizeof(volStr), "%d", settings.volume);
  WiFiManagerParameter volParam("volume", "Volume 1-100", volStr, 4);
  WiFiManagerParameter flipParam("flip", "Flip screen (0 or 1)", settings.flip ? "1" : "0", 2);
  WiFiManagerParameter handsParam("handsfree", "Hands-free, always listening (1) or push-to-talk (0)",
                                  settings.handsFree ? "1" : "0", 2);
  char thrStr[8];
  snprintf(thrStr, sizeof(thrStr), "%d", settings.micThreshold);
  WiFiManagerParameter thrParam("micthr", "Hands-free mic threshold (lower = more sensitive)", thrStr, 6);
  wm.addParameter(&keyParam);
  wm.addParameter(&modelParam);
  wm.addParameter(&volParam);
  wm.addParameter(&flipParam);
  wm.addParameter(&handsParam);
  wm.addParameter(&thrParam);
  wm.setTitle("Gemini Setup");
  wm.setConfigPortalTimeout(forced ? 0 : 300);

  setState(State::Setup, "Join Wi-Fi 'Gemini-Setup' then open 192.168.4.1");
  drawScreen();
  wm.startConfigPortal("Gemini-Setup");

  String key = keyParam.getValue();
  key.trim();
  if (!key.isEmpty()) settings.apiKey = key;
  String model = modelParam.getValue();
  model.trim();
  if (!model.isEmpty()) {
    if (!model.startsWith("models/")) model = "models/" + model;
    settings.model = model;
  }
  settings.volume = constrain(atoi(volParam.getValue()), 1, 100);
  settings.flip = atoi(flipParam.getValue()) == 1;
  settings.handsFree = atoi(handsParam.getValue()) == 1;
  int thr = atoi(thrParam.getValue());
  if (thr > 0) settings.micThreshold = thr;
  saveSettings();
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_TALK, INPUT_PULLUP);
  pinMode(PIN_BOOT, INPUT_PULLUP);

  loadSettings();

  Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL);
  oled.setBusClock(400000);
  oled.begin();
  if (settings.flip) oled.setDisplayRotation(U8G2_R2);

  uint8_t *storage = (uint8_t *)ps_malloc(PLAY_BUF_BYTES + 1);
  playBuf = xStreamBufferCreateStatic(PLAY_BUF_BYTES, 1, storage, &playBufStruct);
  setupI2S();
  xTaskCreatePinnedToCore(playbackTask, "play", 4096, nullptr, 5, nullptr, 1);

  // Offer the setup page for 3 seconds after power-on.
  setState(State::Boot, "Press the button now for setup (Wi-Fi, API key)");
  drawScreen();
  bool wantSetup = false;
  uint32_t t0 = millis();
  while (millis() - t0 < 3000) {
    if (talkPressed()) {
      wantSetup = true;
      break;
    }
    delay(20);
  }
  while (talkPressed()) delay(20);

  WiFi.mode(WIFI_STA);
  if (wantSetup || settings.apiKey.isEmpty()) {
    runSetupPortal(settings.apiKey.isEmpty());
  }

  setState(State::Wifi, "Connecting to saved Wi-Fi");
  drawScreen();
  WiFiManager wm;
  wm.setConfigPortalTimeout(300);
  wm.setEnableConfigPortal(false);
  if (!wm.autoConnect("Gemini-Setup")) {
    // No saved network or it failed: open the full setup page.
    runSetupPortal(true);
  }
  WiFi.setSleep(false);  // lower latency for streaming audio

  if (settings.apiKey.isEmpty()) {
    setState(State::Error, "No API key. Restart and press the button to set it.");
    drawScreen();
    return;
  }
  connectGemini();
  if (settings.handsFree) i2s_start(I2S_MIC);
}

void loop() {
  if (settings.apiKey.isEmpty()) {
    delay(100);
    return;
  }
  ws.loop();

  static bool lastPressed = false;
  static uint32_t lastChange = 0;
  bool pressed = talkPressed();
  if (pressed != lastPressed && millis() - lastChange > 30) {
    lastChange = millis();
    lastPressed = pressed;
    if (pressed && state == State::Error) {
      closeReason = "";
      setState(State::Connecting, "Retrying");
    } else if (settings.handsFree) {
      if (pressed && state == State::Speaking) flushPlayback = true;  // stop talking
    } else if (pressed && sessionReady) {
      startTalking();
    } else if (!pressed && recording) {
      stopTalking();
    }
  }

  if (!playbackIdle()) lastPlaybackMs = millis();

  if (settings.handsFree) {
    readMic();
  } else if (recording) {
    readMic();
  }
  if (!recording && !gateOpen && sessionReady && turnDone && playbackIdle() &&
      (state == State::Speaking || state == State::Thinking)) {
    setState(State::Ready);
  }
  // Hands-free: a noise that was not speech gets no answer; stop waiting.
  if (settings.handsFree && state == State::Thinking && millis() - thinkingSinceMs > 10000) {
    turnDone = true;
    setState(State::Ready);
  }

  // Hands-free: show the mic level while idle, to help tune the threshold.
  static uint32_t lastLevelDraw = 0;
  if (settings.handsFree && state == State::Ready && millis() - lastLevelDraw > 500) {
    lastLevelDraw = millis();
    oled.setDrawColor(0);
    oled.drawBox(0, 54, 128, 10);
    oled.setDrawColor(1);
    oled.setFont(u8g2_font_6x10_tr);
    char buf[32];
    snprintf(buf, sizeof(buf), "Mic %d / %d", micLevel, settings.micThreshold);
    oled.drawStr(0, 63, buf);
    oled.sendBuffer();
  }

  if (WiFi.status() != WL_CONNECTED && state != State::Error) {
    setState(State::Wifi, "Wi-Fi lost, reconnecting");
  }

  drawScreen();
}
