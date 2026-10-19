// Phase 1: INMP441 microphone test.
// Open Tools -> Serial Plotter at 115200 baud and talk or clap.
// Requires the "esp32 by Espressif Systems" board package, version 3.x.

#include <ESP_I2S.h>

const int PIN_MIC_SCK = 4;
const int PIN_MIC_WS  = 5;
const int PIN_MIC_SD  = 6;

const int SAMPLE_RATE = 16000;
const int CHUNK = 512;  // samples per reading

I2SClass i2s;
int32_t samples[CHUNK];

void setup() {
  Serial.begin(115200);
  delay(1000);

  // The mic only sends data, so the ESP32's data-out pin is unused (-1).
  i2s.setPins(PIN_MIC_SCK, PIN_MIC_WS, -1, PIN_MIC_SD);
  // INMP441 sends 24-bit samples inside 32-bit slots; L/R tied to GND = left channel.
  if (!i2s.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_32BIT,
                 I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT)) {
    Serial.println("I2S init failed - check the board package version");
    while (true) delay(1000);
  }
  Serial.println("Mic ready");
}

void loop() {
  size_t bytesRead = i2s.readBytes((char *)samples, sizeof(samples));
  int count = bytesRead / sizeof(int32_t);
  if (count == 0) return;

  // Loudness = average distance from the mean (removes the mic's DC offset).
  int64_t sum = 0;
  for (int i = 0; i < count; i++) sum += samples[i] >> 14;
  int32_t mean = sum / count;

  int64_t level = 0;
  for (int i = 0; i < count; i++) level += abs((samples[i] >> 14) - mean);

  Serial.print("level:");
  Serial.println((int32_t)(level / count));
}
