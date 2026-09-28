// AI_STT recognition test copy.
// Target: original ESP32 38-pin CP2102 board + INMP441.
// This test keeps I2S capture in its own FreeRTOS task so network stalls do
// not immediately stop the microphone. It also uses gentler filtering and
// bounded automatic gain control.

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <driver/i2s.h>
#include <freertos/stream_buffer.h>
#include <math.h>
#include "AI_STT_CONFIG.h"

static WiFiClientSecure net;
static const i2s_port_t I2S_PORT = I2S_NUM_0;
static StreamBufferHandle_t pcmStream;
static TaskHandle_t captureTaskHandle;
static volatile bool captureActive = false;
static volatile bool captureOverrun = false;
static volatile uint32_t capturedBytes = 0;
static volatile uint32_t capturedPeak = 0;
static volatile uint64_t capturedEnergy = 0;
static portMUX_TYPE audioMux = portMUX_INITIALIZER_UNLOCKED;

// 200 ms of pre-roll. This prevents the first consonant from being lost.
static int16_t preRoll[SAMPLE_RATE / 5];
static int16_t preSnapshot[SAMPLE_RATE / 5];
static size_t preRollPos = 0;

static float hpA = 0.0f, hpY = 0.0f, hpX = 0.0f;
static float agcGain = 1.0f;

static void resetAudioStats() {
  portENTER_CRITICAL(&audioMux);
  capturedBytes = 0;
  capturedPeak = 0;
  capturedEnergy = 0;
  captureOverrun = false;
  portEXIT_CRITICAL(&audioMux);
}

static void resetFilter() {
  // A single 75 Hz high-pass preserves more speech fundamentals than the
  // previous two cascaded 120 Hz stages.
  const float cutoff = 75.0f;
  const float rc = 1.0f / (2.0f * PI * cutoff);
  const float dt = 1.0f / SAMPLE_RATE;
  hpA = rc / (rc + dt);
  hpY = hpX = 0.0f;
  agcGain = 1.0f;
}

static void processBlock(int32_t *raw, int16_t *pcm, size_t count) {
  float filtered[256];
  float sumSquares = 0.0f;
  float peak = 0.0f;

  const bool active = captureActive;
  for (size_t i = 0; i < count; ++i) {
    // INMP441 data is 24-bit audio in a signed 32-bit I2S slot.
    const float x = (float)(raw[i] >> 8);
    const float y = hpA * (hpY + x - hpX);
    hpX = x;
    hpY = y;
    filtered[i] = y;
    sumSquares += y * y;
    if (fabsf(y) > peak) peak = fabsf(y);
  }

  const float rms = sqrtf(sumSquares / (float)count);
  // Do not amplify near-silence, otherwise room noise becomes speech.
  float wanted = rms > 220.0f ? 9000.0f / rms : 1.0f;
  wanted = constrain(wanted, 1.0f, 4.0f);
  agcGain = agcGain * 0.85f + wanted * 0.15f;

  for (size_t i = 0; i < count; ++i) {
    int32_t v = (int32_t)(filtered[i] * agcGain);
    v = constrain(v, -32768, 32767);
    pcm[i] = (int16_t)v;
    const uint32_t magnitude = (uint32_t)abs(v);
    if (active) {
      portENTER_CRITICAL(&audioMux);
      if (magnitude > capturedPeak) capturedPeak = magnitude;
      capturedEnergy += (uint64_t)magnitude * magnitude;
      portEXIT_CRITICAL(&audioMux);
    }
  }
}

static void captureTask(void *) {
  int32_t raw[256];
  int16_t pcm[256];

  for (;;) {
    size_t bytesRead = 0;
    if (i2s_read(I2S_PORT, raw, sizeof(raw), &bytesRead, portMAX_DELAY) != ESP_OK)
      continue;

    const size_t samples = bytesRead / sizeof(int32_t);
    processBlock(raw, pcm, samples);

    // Always keep a small rolling pre-roll. It is copied into a request when
    // the user presses BOOT.
    portENTER_CRITICAL(&audioMux);
    for (size_t i = 0; i < samples; ++i) {
      preRoll[preRollPos] = pcm[i];
      preRollPos = (preRollPos + 1) % (sizeof(preRoll) / sizeof(preRoll[0]));
    }
    portEXIT_CRITICAL(&audioMux);

    if (captureActive) {
      const size_t bytes = samples * sizeof(int16_t);
      if (xStreamBufferSend(pcmStream, pcm, bytes, 0) != bytes)
        captureOverrun = true;
      capturedBytes += bytes;
    }
  }
}

static bool ensureLink() {
  if (net.connected()) return true;
  // Development transport retained for this bench test. Production firmware
  // must validate the backend certificate and must not contain provider keys.
  net.setInsecure();
  // TLS negotiation can take longer than one second on the original ESP32.
  net.setTimeout(15000);
  IPAddress resolved;
  if (!WiFi.hostByName(GROQ_HOST, resolved)) {
    Serial.printf("[net] DNS failed for %s; heap=%u\n", GROQ_HOST,
                  (unsigned)ESP.getFreeHeap());
    return false;
  }
  Serial.printf("[net] %s -> %s; heap=%u; opening TLS...\n",
                GROQ_HOST, resolved.toString().c_str(),
                (unsigned)ESP.getFreeHeap());
  const bool connected = net.connect(GROQ_HOST, 443, 15000);
  if (!connected) {
    Serial.printf("[net] HTTPS connect failed; WiFi status=%d RSSI=%d heap=%u\n",
                  (int)WiFi.status(), WiFi.RSSI(), (unsigned)ESP.getFreeHeap());
  }
  return connected;
}

static bool sendChunk(const uint8_t *data, size_t len) {
  char header[16];
  const int hlen = snprintf(header, sizeof(header), "%X\r\n", (unsigned)len);
  return net.write((const uint8_t *)header, hlen) == (size_t)hlen &&
         net.write(data, len) == len &&
         net.write((const uint8_t *)"\r\n", 2) == 2;
}

static void writeWavHeader(uint8_t *h) {
  const uint32_t unknown = 0xFFFFFFFF, rate = SAMPLE_RATE;
  const uint32_t byteRate = rate * 2, fmtLen = 16;
  const uint16_t pcm = 1, channels = 1, blockAlign = 2, bits = 16;
  memcpy(h + 0, "RIFF", 4); memcpy(h + 4, &unknown, 4);
  memcpy(h + 8, "WAVEfmt ", 8); memcpy(h + 16, &fmtLen, 4);
  memcpy(h + 20, &pcm, 2); memcpy(h + 22, &channels, 2);
  memcpy(h + 24, &rate, 4); memcpy(h + 28, &byteRate, 4);
  memcpy(h + 32, &blockAlign, 2); memcpy(h + 34, &bits, 2);
  memcpy(h + 36, "data", 4); memcpy(h + 40, &unknown, 4);
}

static String readLine(uint32_t deadline) {
  String line;
  while (millis() < deadline) {
    if (!net.available()) {
      if (!net.connected()) break;
      delay(2);
      continue;
    }
    const char c = (char)net.read();
    if (c == '\n') break;
    if (c != '\r') line += c;
  }
  return line;
}

static int readReply(String &body, uint32_t timeoutMs) {
  const uint32_t deadline = millis() + timeoutMs;
  int status = 0;
  bool chunked = false;
  for (String line = readLine(deadline); line.length(); line = readLine(deadline)) {
    if (line.startsWith("HTTP/")) status = line.substring(9, 12).toInt();
    line.toLowerCase();
    if (line.startsWith("transfer-encoding:") && line.indexOf("chunked") >= 0)
      chunked = true;
  }
  while (chunked && millis() < deadline) {
    const long count = strtol(readLine(deadline).c_str(), nullptr, 16);
    if (count <= 0) break;
    for (long i = 0; i < count && millis() < deadline; ++i) {
      while (!net.available() && net.connected() && millis() < deadline) delay(2);
      if (net.available()) body += (char)net.read();
    }
    readLine(deadline);
  }
  if (!chunked) {
    while (net.connected() && millis() < deadline) {
      while (!net.available() && net.connected() && millis() < deadline) delay(2);
      while (net.available()) body += (char)net.read();
    }
  }
  return status;
}

static bool transcribe(String &text) {
  if (!ensureLink()) {
    Serial.println("[stt] no HTTPS connection; recording did not start");
    return false;
  }
  xStreamBufferReset(pcmStream);
  resetAudioStats();
  resetFilter();

  net.print(String("POST /openai/v1/audio/transcriptions HTTP/1.1\r\n"
                   "Host: ") + GROQ_HOST + "\r\nAuthorization: Bearer " +
            SEED_GROQ_KEY + "\r\nUser-Agent: AI_STT-test/2.0\r\n"
            "Content-Type: multipart/form-data; boundary=" BOUNDARY "\r\n"
            "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n");

  static const char form[] =
    "--" BOUNDARY "\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n" STT_MODEL "\r\n"
    "--" BOUNDARY "\r\nContent-Disposition: form-data; name=\"language\"\r\n\r\nen\r\n"
    "--" BOUNDARY "\r\nContent-Disposition: form-data; name=\"response_format\"\r\n\r\ntext\r\n"
    "--" BOUNDARY "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"a.wav\"\r\n"
    "Content-Type: audio/wav\r\n\r\n";
  uint8_t wav[44];
  writeWavHeader(wav);
  if (!sendChunk((const uint8_t *)form, strlen(form)) || !sendChunk(wav, sizeof(wav))) {
    net.stop();
    return false;
  }

  // Snapshot the latest 200 ms while capture is idle, then send it before
  // live samples. The ordering is oldest-to-newest around the circular buffer.
  portENTER_CRITICAL(&audioMux);
  const size_t preCount = sizeof(preRoll) / sizeof(preRoll[0]);
  for (size_t i = 0; i < preCount; ++i)
    preSnapshot[i] = preRoll[(preRollPos + i) % preCount];
  portEXIT_CRITICAL(&audioMux);
  for (size_t offset = 0; offset < sizeof(preSnapshot); offset += 1024) {
    const size_t count = min((size_t)1024, sizeof(preSnapshot) - offset);
    if (!sendChunk((const uint8_t *)preSnapshot + offset, count)) {
      net.stop();
      return false;
    }
  }

  captureActive = true;
  Serial.println("[rec] listening...");
  uint32_t start = millis();
  uint8_t packet[1024];

  while (millis() - start < MAX_SECONDS * 1000UL &&
         (digitalRead(PIN_BUTTON) == LOW || millis() - start < 250)) {
    size_t n = xStreamBufferReceive(pcmStream, packet, sizeof(packet), pdMS_TO_TICKS(30));
    if (n && !sendChunk(packet, n)) break;
  }
  captureActive = false;

  // Let the capture task publish the final DMA block, then drain the queue.
  const uint32_t drainUntil = millis() + 350;
  while (millis() < drainUntil) {
    size_t n = xStreamBufferReceive(pcmStream, packet, sizeof(packet), pdMS_TO_TICKS(20));
    if (n && !sendChunk(packet, n)) break;
  }

  const uint32_t peak = capturedPeak;
  const uint64_t energy = capturedEnergy;
  const uint32_t bytes = capturedBytes;
  const uint32_t samples = bytes / sizeof(int16_t);
  const float rms = samples ? sqrtf((float)(energy / samples)) : 0.0f;
  Serial.printf("[rec] %u bytes, peak %u, rms %.0f, overrun %s\n",
                (unsigned)bytes, (unsigned)peak, rms, captureOverrun ? "YES" : "no");

  if (samples < SAMPLE_RATE / 5 || (peak < 500 && rms < 120.0f)) {
    net.stop();
    Serial.println("[rec] no usable speech signal");
    return false;
  }

  String tail = "\r\n--" BOUNDARY "--\r\n";
  if (!sendChunk((const uint8_t *)tail.c_str(), tail.length()) ||
      net.write((const uint8_t *)"0\r\n\r\n", 5) != 5) {
    net.stop();
    return false;
  }
  String body;
  const int status = readReply(body, 20000);
  net.stop();
  body.trim();
  if (status != 200) {
    Serial.printf("[stt] HTTP %d: %s\n", status, body.substring(0, 160).c_str());
    return false;
  }
  text = body;
  return text.length() > 0;
}

void setup() {
  Serial.begin(115200);
  delay(400);
  pinMode(PIN_BUTTON, INPUT_PULLUP);

  i2s_config_t config = {};
  config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  config.sample_rate = SAMPLE_RATE;
  config.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  config.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  config.dma_buf_count = 8;
  config.dma_buf_len = 256;
  config.use_apll = false;
  config.tx_desc_auto_clear = false;
  config.fixed_mclk = 0;

  i2s_pin_config_t pins = {};
  pins.bck_io_num = PIN_MIC_SCK;
  pins.ws_io_num = PIN_MIC_WS;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num = PIN_MIC_SD;
  if (i2s_driver_install(I2S_PORT, &config, 0, nullptr) != ESP_OK ||
      i2s_set_pin(I2S_PORT, &pins) != ESP_OK) {
    Serial.println("[mic] I2S INIT FAILED");
    while (true) delay(1000);
  }

  // Leave enough contiguous heap for WiFiClientSecure's TLS handshake on the
  // original ESP32. 32 KB is about 1.0 s of 16-bit mono PCM at 16 kHz.
  pcmStream = xStreamBufferCreate(32 * 1024, 2);
  if (!pcmStream) {
    Serial.println("[audio] stream buffer allocation failed");
    while (true) delay(1000);
  }
  xTaskCreatePinnedToCore(captureTask, "capture", 4096, nullptr, 2,
                          &captureTaskHandle, 1);
  Serial.println("[audio] I2S capture task started");

  WiFi.mode(WIFI_STA);
  WiFi.begin(SEED_WIFI_SSID, SEED_WIFI_PASS);
  Serial.print("[wifi] connecting");
  while (WiFi.status() != WL_CONNECTED) { Serial.print('.'); delay(300); }
  Serial.printf("\n[wifi] %s\n", WiFi.localIP().toString().c_str());
  Serial.println("[ready] Hold BOOT and speak.");
}

void loop() {
  // Latch one transcription per physical press. This prevents button bounce
  // or a brief HIGH glitch during a failed request from retriggering the job.
  static bool buttonArmed = true;
  static uint32_t highSince = 0;

  const bool pressed = digitalRead(PIN_BUTTON) == LOW;
  if (!pressed) {
    if (!highSince) highSince = millis();
    if (millis() - highSince >= 150) buttonArmed = true;
    delay(10);
    return;
  }

  highSince = 0;
  if (!buttonArmed) {
    delay(10);
    return;
  }

  delay(50);
  if (digitalRead(PIN_BUTTON) != LOW) return;
  buttonArmed = false;
  Serial.println("[button] BOOT pressed; starting transcription");
  String text;
  if (transcribe(text)) {
    Serial.println();
    Serial.println("========== TRANSCRIPTION ==========");
    Serial.print("You said: ");
    Serial.println(text);
    Serial.println("====================================");
    Serial.println();
  } else {
    Serial.println("[stt] transcription failed; see the diagnostic message above");
  }
  // Do not re-arm until the button has been continuously released.
  while (digitalRead(PIN_BUTTON) == LOW) delay(10);
  highSince = millis();
  while (millis() - highSince < 150) {
    if (digitalRead(PIN_BUTTON) == LOW) {
      highSince = 0;
      while (digitalRead(PIN_BUTTON) == LOW) delay(10);
      highSince = millis();
    }
    delay(10);
  }
  buttonArmed = true;
}
