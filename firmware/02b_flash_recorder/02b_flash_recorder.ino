// Phase 2 without an SD card: record to the ESP32's own flash memory (about
// 5 minutes) and download the WAV files over Wi-Fi from a web page.
// Blue LED = ready, red LED = recording. Press the button to start/stop.
//
// Before uploading:
//   - Tools -> Partition Scheme -> "16M Flash (3MB APP/9.9MB FATFS)"
//   - Put your Wi-Fi name and password below (2.4 GHz networks only)
// Requires the "esp32 by Espressif Systems" board package, version 3.x.

#include <ESP_I2S.h>
#include <FFat.h>
#include <WiFi.h>
#include <WebServer.h>
#include <vector>

const char *WIFI_NAME     = "your-wifi-name";
const char *WIFI_PASSWORD = "your-wifi-password";

const int PIN_BUTTON   = 1;
const int PIN_LED_REC  = 2;   // red
const int PIN_LED_IDLE = 42;  // blue

const int PIN_MIC_SCK = 4;
const int PIN_MIC_WS  = 5;
const int PIN_MIC_SD  = 6;

const int SAMPLE_RATE = 16000;
const int CHUNK = 512;
const int GAIN_SHIFT = 14;      // lower = louder (13 is twice as loud as 14)

I2SClass i2s;
WebServer server(80);
File wavFile;
bool recording = false;
uint32_t dataBytes = 0;
uint32_t lastFlushMs = 0;

int32_t raw[CHUNK];
int16_t pcm[CHUNK];

// Simple high-pass filter state, removes the mic's DC offset.
float hpPrevIn = 0, hpPrevOut = 0;

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
    if (!FFat.exists(name)) return String(name);
  }
  return String("/REC_LAST.WAV");
}

void startRecording() {
  String name = nextFileName();
  wavFile = FFat.open(name, FILE_WRITE);
  if (!wavFile) {
    Serial.println("Could not create file - memory full? Delete recordings on the web page.");
    return;
  }
  writeWavHeader(wavFile, 0);  // placeholder, fixed when we stop
  dataBytes = 0;
  hpPrevIn = hpPrevOut = 0;
  lastFlushMs = millis();
  recording = true;
  digitalWrite(PIN_LED_REC, HIGH);
  digitalWrite(PIN_LED_IDLE, LOW);
  Serial.println("Recording to " + name);
}

void stopRecording() {
  recording = false;
  writeWavHeader(wavFile, dataBytes);
  wavFile.close();
  digitalWrite(PIN_LED_REC, LOW);
  digitalWrite(PIN_LED_IDLE, HIGH);
  Serial.printf("Stopped. %.1f seconds saved.\n", dataBytes / 2.0 / SAMPLE_RATE);
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

std::vector<String> listRecordings() {
  std::vector<String> paths;
  File root = FFat.open("/");
  for (File f = root.openNextFile(); f; f = root.openNextFile()) {
    if (!f.isDirectory()) paths.push_back(String(f.path()));
  }
  return paths;
}

void handleRoot() {
  uint32_t freeBytes = FFat.freeBytes();
  String html = "<html><head><meta name='viewport' content='width=device-width'>"
                "<title>Meeting Recorder</title></head>"
                "<body style='font-family:sans-serif;max-width:600px;margin:20px auto'>"
                "<h2>Meeting Recorder</h2>";
  html += recording ? "<p><b style='color:red'>&#9679; Recording...</b> "
                      "Stop recording to download files.</p>"
                    : "<p>Ready.</p>";
  html += "<p>Free space: about " + String(freeBytes / (SAMPLE_RATE * 2) / 60) + " minutes</p><ul>";
  for (const String &path : listRecordings()) {
    File f = FFat.open(path, "r");
    html += "<li><a href='/download?f=" + path + "'>" + path.substring(1) + "</a> (" +
            String(f.size() / (SAMPLE_RATE * 2)) + " s)</li>";
    f.close();
  }
  html += "</ul><p><a href='/deleteall' onclick=\"return confirm('Delete all recordings?')\">"
          "Delete all recordings</a></p></body></html>";
  server.send(200, "text/html", html);
}

void handleDownload() {
  String path = server.arg("f");
  if (recording) {
    server.send(409, "text/plain", "Stop recording first.");
    return;
  }
  if (!path.startsWith("/") || !FFat.exists(path)) {
    server.send(404, "text/plain", "File not found.");
    return;
  }
  File f = FFat.open(path, "r");
  server.sendHeader("Content-Disposition", "attachment; filename=\"" + path.substring(1) + "\"");
  server.streamFile(f, "audio/wav");
  f.close();
}

void handleDeleteAll() {
  if (!recording) {
    for (const String &path : listRecordings()) FFat.remove(path);
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

  if (!FFat.begin(true)) {  // true = format the memory the first time
    Serial.println("Memory failed - set Tools -> Partition Scheme -> 16M Flash (3MB APP/9.9MB FATFS)");
    blinkForever(PIN_LED_REC);  // fast red blink = storage problem
  }

  i2s.setPins(PIN_MIC_SCK, PIN_MIC_WS, -1, PIN_MIC_SD);
  if (!i2s.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_32BIT,
                 I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT)) {
    Serial.println("Microphone (I2S) failed to start");
    blinkForever(PIN_LED_IDLE);  // fast blue blink = mic problem
  }

  connectWifi();
  server.on("/", handleRoot);
  server.on("/download", handleDownload);
  server.on("/deleteall", handleDeleteAll);
  server.begin();

  digitalWrite(PIN_LED_IDLE, HIGH);
  Serial.printf("Ready. About %u minutes of space. Press the button to record.\n",
                FFat.freeBytes() / (SAMPLE_RATE * 2) / 60);
}

void loop() {
  server.handleClient();

  if (buttonPressed()) {
    if (recording) stopRecording();
    else startRecording();
  }

  // Always read the mic so the buffer stays fresh, even when not recording.
  size_t bytesRead = i2s.readBytes((char *)raw, sizeof(raw));
  int count = bytesRead / sizeof(int32_t);
  if (!recording || count == 0) return;

  for (int i = 0; i < count; i++) {
    float x = (float)(raw[i] >> GAIN_SHIFT);
    float y = x - hpPrevIn + 0.995f * hpPrevOut;
    hpPrevIn = x;
    hpPrevOut = y;
    if (y > 32767) y = 32767;
    if (y < -32768) y = -32768;
    pcm[i] = (int16_t)y;
  }

  size_t written = wavFile.write((uint8_t *)pcm, count * sizeof(int16_t));
  dataBytes += written;

  // Save every 5 s, and stop automatically before the memory is full.
  if (millis() - lastFlushMs > 5000) {
    wavFile.flush();
    lastFlushMs = millis();
    if (FFat.freeBytes() < 64 * 1024) {
      Serial.println("Memory full.");
      stopRecording();
    }
  }
}
