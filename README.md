# Master Sword Keychain

A tiny animated keychain built on the Waveshare ESP32-S3-Touch-AMOLED-1.64 board. A glowing sword hovers on screen with falling weather (rain, snow, autumn leaves, or spring petals — rotating every couple of minutes), the occasional lightning strike, medieval-flavored typewriter text, real battery monitoring, shake-to-wake, and a manual "charging" animation.

![status](https://img.shields.io/badge/status-working-brightgreen) ![board](https://img.shields.io/badge/board-ESP32--S3--Touch--AMOLED--1.64-blue)

## Features

- Animated sword with a gentle idle float
- Four rotating weather types — rain, snow, autumn leaves, spring petals — each with its own fall speed, sway, and color, changing every 2 minutes
- Occasional procedural lightning strikes at irregular intervals
- Typewriter-animated medieval quotes, auto-wrapped to fit the screen
- Real battery percentage from the onboard ADC (checked every 3 minutes to save power)
- Shake-to-wake — tuned to require a deliberate shake, not just a touch
- Single press = wake, double-tap = toggle a "charging" animation, long-press = power off (deep sleep)
- Auto-sleeps after a minute of inactivity to save battery
- Built-in performance profiler (prints frame timing to Serial every 3s)

## Hardware required

- **Waveshare ESP32-S3-Touch-AMOLED-1.64** (this project targets the **V2** hardware revision specifically — V1 and V2 swap a couple of pins; see [Board setup](#board-setup) below)
- USB-C cable
- Optional: a single-cell LiPo battery (connects to the board's onboard `BAT` connector)

## Software required

| Tool | Notes |
|---|---|
| [Arduino IDE 2.x](https://www.arduino.cc/en/software) | This project was built and tested on IDE 2.x |
| ESP32 board package (Espressif Systems), **v3.3.11 or newer** | Installed via Boards Manager — must be new enough to include the Waveshare AMOLED board variant |
| [TFT_eSPI](https://github.com/Bodmer/TFT_eSPI) library | Used only as an in-memory 2D drawing toolkit (fonts, shapes, sprite compositing) — never talks to the display hardware directly, see [Why esp_lcd_sh8601.c/.h exist](#why-esp_lcd_sh8601ch-exist) |
| [TJpg_Decoder](https://github.com/Bodmer/TJpg_Decoder) library | JPEG decoding for the sword image |
| [arduino-littlefs-upload](https://github.com/earlephilhower/arduino-littlefs-upload) plugin | Needed to upload the image file separately from the sketch — Arduino IDE 2.x has no built-in equivalent, see [Uploading the image](#3-upload-the-image) |

## Repository structure

```
Keychain2/
├── Keychain2.ino        ← main sketch
├── esp_lcd_sh8601.c      ← display panel driver (must stay next to the .ino)
├── esp_lcd_sh8601.h
└── data/
    └── (put your sword image here — not included, see below)
```

Arduino requires the sketch's containing folder to be named exactly the same as the `.ino` file, so don't rename one without the other.

### Why esp_lcd_sh8601.c/.h exist

This board's display (an SH8601 driver chip over a QSPI interface) isn't supported by stock TFT_eSPI. These two files are a real ESP-IDF panel driver, adapted from Waveshare's own example for this exact board, which does the actual job of getting pixels onto the screen. TFT_eSPI is still used, but purely as a software framebuffer/font engine — it never touches the display hardware itself.

## Board setup

1. In Arduino IDE, go to **File → Preferences** and add the Espressif ESP32 boards manager URL if you haven't already (`https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`).
2. **Tools → Board → Boards Manager**, install **esp32 by Espressif Systems**, version **3.3.11 or later**.
3. **Tools → Board**, select the Waveshare ESP32-S3-Touch-AMOLED-1.64 board entry.
4. Set the rest of the Tools menu:
   - **Flash Size:** 4MB
   - **Partition Scheme:** a 4MB scheme that includes SPIFFS/LittleFS space (e.g. *"Default 4MB with spiffs (1.2MB APP/1.5MB SPIFFS)"*) — this project genuinely needs the flash and partition settings to match, otherwise the bootloader will fail to even load a valid partition table
   - **PSRAM:** OPI PSRAM (enabled) — the display's frame buffers live in PSRAM
   - **Port:** whichever COM port / /dev/tty your board shows up as

## Installing the image

Only one sword image ships with this project's logic (multi-image rotation exists in the code but is intentionally disabled — see [Re-enabling multiple images](#re-enabling-multiple-sword-images)).

1. Your image **must be exactly `SWORD_WIDTH` × `SWORD_HEIGHT`** pixels as defined near the top of `Keychain2.ino` (currently **102×249**). The code does not resize images — if your file's real dimensions don't match these numbers exactly, you'll get a cropped/misaligned result.
2. Save it as `sword0.jpg` inside the `Keychain2/data/` folder (replacing the placeholder text file there).
3. Update `SWORD_WIDTH`/`SWORD_HEIGHT` in the sketch if you use a different size.

### Uploading the image

Arduino IDE 2.x has no built-in LittleFS uploader, so you need a plugin:

1. Download the latest `.vsix` from [arduino-littlefs-upload releases](https://github.com/earlephilhower/arduino-littlefs-upload/releases).
2. Move it into `<your home folder>/.arduinoIDE/plugins/` (create the folder if it doesn't exist), then fully restart Arduino IDE.
3. With `Keychain2.ino` open and your image in place inside `data/`, close Serial Monitor if it's open, then run **Ctrl+Shift+P → "Upload LittleFS to Pico/ESP8266/ESP32"**.
4. Then do a normal **Upload** of the sketch itself. The two uploads are independent — changing the sketch doesn't touch the filesystem, and vice versa.

## Controls

| Action | Result |
|---|---|
| Short press BOOT | Wake the display if it's asleep |
| Double-tap BOOT (within 400ms) | Toggle the manual charging-screen animation on/off |
| Hold BOOT for 1.5s+ | Power off (deep sleep) — press BOOT again to turn back on (triggers a full reboot) |
| Shake the device | Wakes it from sleep (tuned to need a real shake, not just handling it) |

## Settings reference

All of these are `#define`s near the top of `Keychain2.ino` — safe to tune without touching any other logic.

| Setting | Current value | Controls |
|---|---|---|
| `SWORD_WIDTH` / `SWORD_HEIGHT` | 102 × 249 | Must exactly match your image file's real pixel dimensions |
| `INACTIVITY_TIMEOUT_MS` | 60000 (1 min) | How long with no interaction before the display sleeps |
| `LONG_PRESS_MS` | 1500 | How long to hold BOOT to power off |
| `DOUBLE_TAP_WINDOW_MS` | 400 | Max gap between taps to count as a double-tap |
| `SHAKE_THRESHOLD` | 3.0 | How strong a motion spike needs to be to count toward a shake |
| `SHAKE_REQUIRED_SAMPLES` | 3 | Consecutive over-threshold readings needed (filters out a single tap) |
| `RAIN_VARIATION_INTERVAL_MS` | 30000 (30s) | How often particle speed/density re-rolls |
| `WEATHER_CHANGE_INTERVAL_MS` | 120000 (2 min) | How often the weather type (rain/snow/leaves/spring) rotates |
| `BATTERY_EMPTY_V` / `BATTERY_FULL_V` | 3.0 / 4.2 | Voltage range used to estimate battery percentage (linear approximation, not a true fuel gauge) |
| `BATTERY_CHECK_INTERVAL_MS` | 3 min | How often the battery ADC is actually read |
| `CPU_MHZ_ACTIVE` / `CPU_MHZ_IDLE` | 240 / 80 | CPU clock speed while awake vs. asleep |
| `TARGET_FRAME_MS` | 33 (~30fps) | Frame rate cap |
| `USE_LIGHT_SLEEP` | 0 (off) | Experimental deeper idle sleep — off by default, see the comment above it in the code before enabling |

The medieval quotes live in the `quotes[]` array (auto-wrapped to fit the screen, no manual line breaks needed) — edit freely, any length works.

### Re-enabling multiple sword images

The code originally supported randomly rotating between several sword images on wake, but that's disabled while only one image is in use. To bring it back:

1. Add more image paths to the `swordImages[]` array (e.g. `"/sword1.jpg"`, `"/sword2.jpg"`, ...) and upload the matching files into `data/`.
2. Search the sketch for `selectRandomSword()` — uncomment the function definition (wrapped in `/* */`) and its two call sites (one in `setup()`, one in `wakeDisplay()`).

## Known limitations

Being upfront about a few things rather than overselling them:

- **Battery percentage is a rough estimate**, linearly mapped between two voltage thresholds — not a real fuel-gauge chip reading. It'll be closest to accurate mid-discharge and least accurate right at the very top/bottom of the range.
- **There's no automatic USB/charging detection.** This board doesn't expose a charge-status signal to the ESP32 in software (confirmed against the board's pinout) — the physical **CHG** LED near the USB-C port is the real charging indicator. The on-screen "charging screen" is a manually-toggled animation (double-tap), not automatic.
- **Weather physics, lightning frequency, and shake sensitivity are first-pass tuned values.** They may want small adjustments depending on how your specific unit is mounted/carried.

## Credits

- Display driver adapted from Waveshare's own example for the ESP32-S3-Touch-AMOLED-1.64 board.
- QMI8658 register configuration based on the public QST QMI8658A/C datasheet.
