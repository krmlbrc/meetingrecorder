# Meeting Recorder

A pocket gadget that records meetings and turns them into AI summaries.

**How it works**

```
[ INMP441 mic ] --I2S--> [ ESP32-S3 ] --SPI--> [ microSD card ]   (WAV files, always kept as backup)
                            |   |  \--I2C--> [ DS3231 clock ]     (timestamps)
                 button + LEDs  \--SPI--> [ 2.13" e-paper ]       (status screen)
                            |
                          Wi-Fi (live audio stream)
                            v
        [ your laptop ]  transcribe (Whisper)  ->  who spoke when (pyannote)
                            |                  ->  summarize (Claude)
                            v
        [ Google Doc ]   summary, key points, decisions, action items with owners
                         (open it in the Google Docs app on your phone; it updates live)
```

The device only records and streams; the heavy AI work runs on your laptop.
That keeps the electronics simple and the battery small. No paid server is
needed: Google hosts the Doc, so you can read it on your phone from anywhere.
The only running cost is the Claude API (pay per use, typically cents per meeting).

## Roadmap

| Phase | Goal | Status |
|-------|------|--------|
| 0 | Install Arduino IDE, make the ESP32-S3 blink an LED | ✅ done |
| 1 | Microphone test: see your voice as a wave on screen | ✅ done (`firmware/01_mic_test`) |
| 2 | Press the button to record a WAV file to the SD card | in progress (`firmware/02_sd_recorder`; no card yet: `firmware/02b_flash_recorder`) |
| 3 | Laptop script: WAV → transcript with speakers → Claude summary → Google Doc | to do |
| 4 | Recognize regular participants by voice, so "Speaker 2" becomes "Ahmet" | to do |
| 5 | Live mode: device streams audio to the laptop, the Doc updates every ~2 min | to do |
| 6 | Real clock (DS3231) for file names, e-paper status screen | to do |
| 7 | Battery + charger + on/off switch | to do |
| 8 | Works anywhere via your phone's hotspot, then solder onto perfboard and put it in a case | to do |

Phases 3–5 are laptop software and need no new wiring, so they can be done
before or alongside the hardware phases 6–8.

Start with **[BUILD_GUIDE.md](BUILD_GUIDE.md)**.
