// Phase 2: press the button to start/stop recording a WAV file to the SD card.
// Blue LED = ready, red LED = recording. Files: REC_0001.WAV, REC_0002.WAV, ...
// Recordings can be downloaded over Wi-Fi from a web page, so no card reader is needed.
//
// Before uploading, put your Wi-Fi name and password below (2.4 GHz networks only).
// Requires the "esp32 by Espressif Systems" board package, version 3.x, and
// Tools -> PSRAM -> "OPI PSRAM" (used as a large audio buffer).
//
// While recording, the Serial Monitor (115200) prints a status line every 10 s
// and a summary when you stop. If a recording comes out short, send those lines.

#include <ESP_I2S.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <WebServer.h>
#include <freertos/stream_buffer.h>
#include <vector>

const char *WIFI_NAME     = "your-wifi-name";
const char *WIFI_PASSWORD = "your-wifi-password";

const int PIN_BUTTON   = 1;
const int PIN_LED_REC  = 2;   // red
const int PIN_LED_IDLE = 42;  // blue

const int PIN_MIC_SCK = 4;
const int PIN_MIC_WS  = 5;
const int PIN_MIC_SD  = 6;

const int PIN_SD_CS   = 10;
const int PIN_SD_MOSI = 11;
const int PIN_SD_SCK  = 12;
const int PIN_SD_MISO = 13;

const int SAMPLE_RATE = 16000;  // 16 kHz mono is what speech recognition expects
const int CHUNK = 512;          // samples per microphone read
const int GAIN_SHIFT = 14;      // lower = louder (13 is twice as loud as 14)
const uint32_t SD_SPEED = 1000000;  // 1 MHz: proven reliable over jumper wires

// SD cards pause now and then while they write, sometimes for half a second or
// more. The microphone runs in its own task and fills this buffer, so those
// pauses don't lose audio. 2 MB of PSRAM holds about a minute.
const size_t AUDIO_BUF_PSRAM    = 2 * 1024 * 1024;
const size_t AUDIO_BUF_NO_PSRAM = 64 * 1024;  // about 2 s, if PSRAM is off
const size_t SD_WRITE_SIZE      = 4096;       // write to the card in 4 KB pieces

I2SClass i2s;
WebServer server(80);
File wavFile;
String recName;
volatile bool recording = false;

StreamBufferHandle_t audioBuf;
StaticStreamBuffer_t audioBufState;
size_t audioBufSize;
uint8_t sdChunk[SD_WRITE_SIZE];

uint32_t dataBytes = 0;   // audio bytes in the file
uint32_t recStartMs = 0;
uint32_t lastFlushMs = 0;
uint32_t lastReportMs = 0;
uint32_t slowestWriteMs = 0;
int writeErrors = 0;
volatile uint32_t droppedBytes = 0;  // audio lost because the buffer was full

void writeWavHeader(File &f, uint32_t dataLen) {
  uint32_t byteRate = SAMPLE_RATE * 2;
  uint8_t h[44] = {
    'R','I','F','F', 0,0,0,0, 'W','A','V','E',
    'f','m','t',' ', 16,0,0,0, 1,0, 1,0,   // PCM, 1 channel
    0,0,0,0, 0,0,0,0, 2,0, 16,0,           // sample rate, byte rate, block align, 16 bit
    'd','a','t','a', 0,0,0,0
  };
  uint32_t riffLen = dataLen + 36;
  memcpy(h + 4, &riffLen, 4);
  uint32_t sr = SAMPLE_RATE;
  memcpy(h + 24, &sr, 4);
  memcpy(h + 28, &byteRate, 4);
  memcpy(h + 40, &dataLen, 4);
  f.seek(0);
  f.write(h, sizeof(h));
}

String nextFileName() {
  char name[20];
  for (int n = 1; n < 10000; n++) {
    snprintf(name, sizeof(name), "/REC_%04d.WAV", n);
    if (!SD.exists(name)) return String(name);
  }
  return String("/REC_LAST.WAV");
}

// Runs on its own: reads the microphone, filters it and queues it for the SD card.
void micTask(void *) {
  static int32_t raw[CHUNK];
  static int16_t pcm[CHUNK];
  float hpPrevIn = 0, hpPrevOut = 0;  // simple high-pass filter, removes the mic's DC offset
  bool wasRecording = false;

  while (true) {
    size_t bytesRead = i2s.readBytes((char *)raw, sizeof(raw));
    int count = bytesRead / sizeof(int32_t);
    if (count == 0) {
      vTaskDelay(1);
      continue;
    }
    if (!recording) {
      wasRecording = false;
      continue;
    }
    if (!wasRecording) {
      hpPrevIn = hpPrevOut = 0;
      wasRecording = true;
    }

    for (int i = 0; i < count; i++) {
      float x = (float)(raw[i] >> GAIN_SHIFT);
      float y = x - hpPrevIn + 0.995f * hpPrevOut;
      hpPrevIn = x;
      hpPrevOut = y;
      if (y > 32767) y = 32767;
      if (y < -32768) y = -32768;
      pcm[i] = (int16_t)y;
    }

    size_t bytes = count * sizeof(int16_t);
    droppedBytes += bytes - xStreamBufferSend(audioBuf, pcm, bytes, 0);
  }
}

void noteWriteTime(uint32_t startMs) {
  uint32_t took = millis() - startMs;
  if (took > slowestWriteMs) slowestWriteMs = took;
}

// Opens the recording again after a write error and continues after the last
// audio that was safely saved (at most a few seconds before the error).
bool reopenFile() {
  wavFile.close();
  wavFile = SD.open(recName, "r+");
  if (!wavFile) {
    // The card itself may need a restart.
    SD.end();
    SPI.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
    if (SD.begin(PIN_SD_CS, SPI, SD_SPEED)) wavFile = SD.open(recName, "r+");
  }
  if (!wavFile) return false;

  uint32_t size = wavFile.size();
  if (size < 44) {
    writeWavHeader(wavFile, 0);
    size = 44;
  }
  dataBytes = (size - 44) & ~1u;  // whole 16-bit samples only
  wavFile.seek(44 + dataBytes);
  return true;
}

// Writes the first len bytes of sdChunk to the file. After one failed write the
// SD library refuses every later write to the same file, which used to cut
// recordings short without any sign, so reopen the file and try again.
bool writeToCard(size_t len) {
  for (int attempt = 0; attempt < 3; attempt++) {
    uint32_t startMs = millis();
    size_t written = wavFile.write(sdChunk, len);
    noteWriteTime(startMs);
    if (written == len) {
      dataBytes += len;
      return true;
    }
    writeErrors++;
    Serial.printf("SD write error after %u s of recording, reopening the file\n",
                  (unsigned)((millis() - recStartMs) / 1000));
    if (!reopenFile()) break;
  }
  Serial.println("The SD card stopped responding, recording stopped. "
                 "See \"Troubleshooting: SD card problems\" in BUILD_GUIDE.md.");
  return false;
}

void startRecording() {
  recName = nextFileName();
  wavFile = SD.open(recName, FILE_WRITE);
  if (!wavFile) {
    Serial.println("Could not create file on SD card");
    return;
  }
  writeWavHeader(wavFile, 0);  // placeholder, fixed when we stop

  // Throw away anything left over from the last recording.
  while (xStreamBufferReceive(audioBuf, sdChunk, SD_WRITE_SIZE, 0) > 0) {}
  dataBytes = 0;
  writeErrors = 0;
  slowestWriteMs = 0;
  droppedBytes = 0;
  recStartMs = lastFlushMs = lastReportMs = millis();
  recording = true;
  digitalWrite(PIN_LED_REC, HIGH);
  digitalWrite(PIN_LED_IDLE, LOW);
  Serial.println("Recording to " + recName);
}

void stopRecording(bool cardOk) {
  recording = false;
  float recordedS = (millis() - recStartMs) / 1000.0;
  // Save the audio that is still waiting in the buffer.
  size_t len;
  while (cardOk && (len = xStreamBufferReceive(audioBuf, sdChunk, SD_WRITE_SIZE, 0)) > 0) {
    cardOk = writeToCard(len);
  }
  writeWavHeader(wavFile, dataBytes);
  wavFile.close();
  digitalWrite(PIN_LED_REC, LOW);
  digitalWrite(PIN_LED_IDLE, HIGH);

  float savedS = dataBytes / 2.0 / SAMPLE_RATE;
  Serial.printf("Stopped. %.1f seconds saved out of %.1f seconds recorded.\n", savedS, recordedS);
  Serial.printf("SD write errors: %d, slowest SD write: %u ms, lost to a full buffer: %.1f s\n",
                writeErrors, (unsigned)slowestWriteMs, droppedBytes / 2.0 / SAMPLE_RATE);
  if (savedS < recordedS - 1) {
    Serial.println("Some audio is missing. Copy these lines and send them for help.");
  }
}

bool buttonPressed() {
  static bool lastState = HIGH;
  static uint32_t lastChangeMs = 0;
  bool state = digitalRead(PIN_BUTTON);
  if (state != lastState && millis() - lastChangeMs > 50) {
    lastChangeMs = millis();
    lastState = state;
    return state == LOW;  // pressed = connected to GND
  }
  return false;
}

void blinkForever(int pin) {
  pinMode(pin, OUTPUT);
  while (true) {
    digitalWrite(pin, !digitalRead(pin));
    delay(200);
  }
}

// Files in the top folder of the card (sub-folders are left alone).
std::vector<String> listFiles() {
  std::vector<String> paths;
  File root = SD.open("/");
  for (File f = root.openNextFile(); f; f = root.openNextFile()) {
    if (!f.isDirectory()) paths.push_back(String(f.path()));
  }
  return paths;
}

void handleRoot() {
  uint64_t freeMB = (SD.totalBytes() - SD.usedBytes()) / (1024 * 1024);
  String html = "<html><head><meta name='viewport' content='width=device-width'>"
                "<title>Meeting Recorder</title></head>"
                "<body style='font-family:sans-serif;max-width:600px;margin:20px auto'>"
                "<h2>Meeting Recorder</h2>";
  html += recording ? "<p><b style='color:red'>&#9679; Recording...</b> "
                      "Stop recording to download files.</p>"
                    : "<p>Ready.</p>";
  html += "<p>Free space: " + String((uint32_t)freeMB) + " MB (about " +
          String((uint32_t)(freeMB * 1024 * 1024 / (SAMPLE_RATE * 2) / 3600)) + " hours)</p><ul>";
  for (const String &path : listFiles()) {
    File f = SD.open(path, "r");
    html += "<li><a href='/download?f=" + path + "'>" + path.substring(1) + "</a> (" +
            String((uint32_t)(f.size() / 1024)) + " KB)</li>";
    f.close();
  }
  html += "</ul><p><a href='/deleteall' onclick=\"return confirm('Delete all files on the card?')\">"
          "Delete all files</a></p></body></html>";
  server.send(200, "text/html", html);
}

void handleDownload() {
  String path = server.arg("f");
  if (recording) {
    server.send(409, "text/plain", "Stop recording first.");
    return;
  }
  if (!path.startsWith("/") || !SD.exists(path)) {
    server.send(404, "text/plain", "File not found.");
    return;
  }
  File f = SD.open(path, "r");
  String type = path.endsWith(".WAV") || path.endsWith(".wav") ? "audio/wav" : "application/octet-stream";
  server.sendHeader("Content-Disposition", "attachment; filename=\"" + path.substring(1) + "\"");
  server.streamFile(f, type);
  f.close();
}

void handleDeleteAll() {
  if (!recording) {
    for (const String &path : listFiles()) SD.remove(path);
  }
  server.sendHeader("Location", "/");
  server.send(303);
}

void connectWifi() {
  Serial.printf("Connecting to Wi-Fi \"%s\"", WIFI_NAME);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_NAME, WIFI_PASSWORD);
  for (int i = 0; i < 30 && WiFi.status() != WL_CONNECTED; i++) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Wi-Fi connected. Open this in your browser: http://" + WiFi.localIP().toString());
  } else {
    Serial.println("Wi-Fi failed - check name/password (2.4 GHz only). Recording still works.");
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(PIN_LED_REC, OUTPUT);
  pinMode(PIN_LED_IDLE, OUTPUT);

  SPI.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
  if (!SD.begin(PIN_SD_CS, SPI, SD_SPEED)) {
    Serial.println("SD card failed - check wiring and that the card is FAT32");
    blinkForever(PIN_LED_REC);  // fast red blink = SD problem
  }
  Serial.printf("SD card OK: %u MB\n", (uint32_t)(SD.cardSize() / (1024 * 1024)));

  audioBufSize = AUDIO_BUF_PSRAM;
  uint8_t *storage = (uint8_t *)ps_malloc(audioBufSize + 1);
  if (!storage) {
    Serial.println("No PSRAM - set Tools -> PSRAM -> OPI PSRAM. Using a small audio buffer.");
    audioBufSize = AUDIO_BUF_NO_PSRAM;
    storage = (uint8_t *)malloc(audioBufSize + 1);
  }
  audioBuf = xStreamBufferCreateStatic(audioBufSize, 1, storage, &audioBufState);

  i2s.setPins(PIN_MIC_SCK, PIN_MIC_WS, -1, PIN_MIC_SD);
  if (!i2s.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_32BIT,
                 I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT)) {
    Serial.println("Microphone (I2S) failed to start");
    blinkForever(PIN_LED_IDLE);  // fast blue blink = mic problem
  }
  // Higher priority than loop(), so the mic is read even while the card is busy.
  xTaskCreate(micTask, "mic", 4096, NULL, 5, NULL);

  connectWifi();
  server.on("/", handleRoot);
  server.on("/download", handleDownload);
  server.on("/deleteall", handleDeleteAll);
  server.begin();

  digitalWrite(PIN_LED_IDLE, HIGH);
  Serial.println("Ready. Press the button to record.");
}

void loop() {
  server.handleClient();

  if (buttonPressed()) {
    if (recording) stopRecording(true);
    else startRecording();
  }

  if (!recording) return;

  // The mic task fills the buffer; move it to the card in 4 KB pieces.
  if (xStreamBufferBytesAvailable(audioBuf) >= SD_WRITE_SIZE) {
    size_t len = xStreamBufferReceive(audioBuf, sdChunk, SD_WRITE_SIZE, 0);
    if (!writeToCard(len)) {
      stopRecording(false);
      return;
    }
  } else {
    delay(1);
  }

  // Save to the card every 5 s so a power cut loses at most a few seconds.
  if (millis() - lastFlushMs > 5000) {
    uint32_t startMs = millis();
    wavFile.flush();
    noteWriteTime(startMs);
    lastFlushMs = millis();
  }

  if (millis() - lastReportMs >= 10000) {
    lastReportMs += 10000;
    Serial.printf("REC %u s: %.1f s saved, buffer %u%% full, slowest SD write %u ms, errors %d\n",
                  (unsigned)((millis() - recStartMs) / 1000), dataBytes / 2.0 / SAMPLE_RATE,
                  (unsigned)(xStreamBufferBytesAvailable(audioBuf) * 100 / audioBufSize),
                  (unsigned)slowestWriteMs, writeErrors);
  }
}
