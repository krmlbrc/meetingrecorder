# Meeting Recorder

A pocket gadget that records meetings and turns them into AI summaries.

**How it works**

```
[ INMP441 mic ] --I2S--> [ ESP32-S3 ] --SPI--> [ microSD card ]   (WAV files)
                            |   |  \--I2C--> [ DS3231 clock ]     (timestamps)
                 button + LEDs  \--SPI--> [ 2.13" e-paper ]       (status screen)
                            |
                          Wi-Fi
                            v
        [ your laptop ]  transcribe (Whisper)  ->  summarize (Claude)  ->  notes.md
```

The device only records; the heavy AI work runs on your laptop. That keeps the
electronics simple and the battery small.

## Roadmap

| Phase | Goal | Status |
|-------|------|--------|
| 0 | Install Arduino IDE, make the ESP32-S3 blink an LED | |
| 1 | Microphone test: see your voice as a wave on screen | `firmware/01_mic_test` |
| 2 | Press the button to record a WAV file to the SD card | `firmware/02_sd_recorder` |
| 3 | Laptop script: WAV → transcript → summary | to do |
| 4 | Real clock (DS3231) for file names, e-paper status screen | to do |
| 5 | Battery + charger + on/off switch | to do |
| 6 | Wi-Fi upload, then solder onto perfboard and put it in a case | to do |

Start with **[BUILD_GUIDE.md](BUILD_GUIDE.md)**.
