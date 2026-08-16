#include <Wire.h>
#include <TFT_eSPI.h> 
#include <math.h>

// --- FLASH FILESYSTEM & JPEG LIBRARIES ---
#include <FS.h>
#include <LittleFS.h>
#include <TJpg_Decoder.h>

// IMU is back, but scoped narrowly: shake-to-wake ONLY, checked only while
// asleep. No tilt/parallax, no continuous polling while the animation is
// running - a much smaller footprint than the full gyro system that was
// removed earlier.

// --- REAL DISPLAY DRIVER (ESP-IDF esp_lcd, matches Waveshare's own working
// example for this exact SH8601 QSPI AMOLED panel) ---
// esp_lcd_sh8601.c / esp_lcd_sh8601.h must sit in this sketch's folder.
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_sh8601.h"

// --- BATTERY SENSING (ADC1 channel 3 / GPIO4, verified against Waveshare's
// own 01_ADC_Test example for this board - adc_bsp.c/h) ---
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

#define SCREEN_WIDTH  280
#define SCREEN_HEIGHT 456

// --- POWER MANAGEMENT ---
#include "esp_sleep.h"

TFT_eSPI tft = TFT_eSPI(); 
TFT_eSprite sprite = TFT_eSprite(&tft);
// NOTE: tft.begin() is never called. TFT_eSPI is used ONLY as a software
// framebuffer/2D graphics toolkit here (fillSprite, drawLine, drawString,
// pushImage, pushToSprite - all pure in-memory operations). The actual
// hardware SPI/QSPI code inside TFT_eSPI is never invoked, because it isn't
// built for this panel's QSPI interface. The real panel driver below is.

// --- Display panel pins (from Waveshare's own lcd_config.h for this board) ---
#define LCD_PIN_CS    46
#define LCD_PIN_PCLK  10
#define LCD_PIN_D0    11
#define LCD_PIN_D1    12
#define LCD_PIN_D2    13
#define LCD_PIN_D3    14
#define LCD_PIN_RST   21
#define LCD_HOST      SPI2_HOST

esp_lcd_panel_io_handle_t lcd_io_handle = NULL;
esp_lcd_panel_handle_t lcd_panel_handle = NULL;

// Panel init sequence, copied from Waveshare's own working lcd_bsp.c for this
// exact board/panel - do not reorder or "clean up", these are vendor-specific
// register writes the panel needs in this exact sequence.
static const sh8601_lcd_init_cmd_t lcd_init_cmds[] = {
  {0x11, (uint8_t[]){0x00}, 0, 80},
  {0xC4, (uint8_t[]){0x80}, 1, 0},
  {0x35, (uint8_t[]){0x00}, 1, 0},
  {0x53, (uint8_t[]){0x20}, 1, 1},
  {0x63, (uint8_t[]){0xFF}, 1, 1},
  {0x51, (uint8_t[]){0x00}, 1, 1},
  {0x29, (uint8_t[]){0x00}, 0, 10},
  {0x51, (uint8_t[]){0xFF}, 1, 0},
};

static bool lcdColorTransDone(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx) {
  return false; // no LVGL/async flush here, nothing to notify
}

// Sends a bare command (no data) using the same QSPI command encoding the
// sh8601 driver uses internally - needed for Sleep In/Out (0x10/0x11), which
// the driver doesn't expose a dedicated function for.
esp_err_t lcdSendCmd(uint8_t cmd) {
  uint32_t encoded = ((uint32_t)cmd & 0xff) << 8 | (0x02UL << 24); // 0x02 = QSPI write-command opcode
  return esp_lcd_panel_io_tx_param(lcd_io_handle, encoded, NULL, 0);
}

void initDisplay() {
  spi_bus_config_t buscfg = SH8601_PANEL_BUS_QSPI_CONFIG(
      LCD_PIN_PCLK, LCD_PIN_D0, LCD_PIN_D1, LCD_PIN_D2, LCD_PIN_D3,
      SCREEN_WIDTH * SCREEN_HEIGHT * 2);
  ESP_ERROR_CHECK_WITHOUT_ABORT(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

  esp_lcd_panel_io_spi_config_t io_config = SH8601_PANEL_IO_QSPI_CONFIG(LCD_PIN_CS, lcdColorTransDone, NULL);
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &lcd_io_handle));

  sh8601_vendor_config_t vendor_config = {
    .init_cmds = lcd_init_cmds,
    .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]),
    .flags = { .use_qspi_interface = 1 },
  };
  esp_lcd_panel_dev_config_t panel_config = {
    .reset_gpio_num = LCD_PIN_RST,
    .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
    .bits_per_pixel = 16,
    .vendor_config = &vendor_config,
  };
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_new_panel_sh8601(lcd_io_handle, &panel_config, &lcd_panel_handle));
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_reset(lcd_panel_handle));
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_init(lcd_panel_handle));
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_set_gap(lcd_panel_handle, 0x14, 0)); // panel memory offset, per Waveshare's flush callback
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_disp_on_off(lcd_panel_handle, true));
}

#define BUTTON_PIN    0      // BOOT button: short press = wake, long press = power off
#define INACTIVITY_TIMEOUT_MS 60000 // was 30000

// --- I2C / IMU PINS (verified against Waveshare's own lcd_config.h) ---
#define I2C_SDA 47
#define I2C_SCL 48


// --- FRAME RATE CAP ---
// Uncapped loops render as fast as they possibly can, which burns CPU (and
// battery) for zero visible benefit. 30fps is plenty smooth for this UI.
#define TARGET_FRAME_MS 33

// --- CPU CLOCK SCALING ---
// Full speed while actively rendering, throttled down while the screen is asleep.
#define CPU_MHZ_ACTIVE 240
#define CPU_MHZ_IDLE   80

// --- OPTIONAL: TRUE LIGHT SLEEP WHILE IDLE ---
// Off by default. Once you've confirmed everything above works well on your
// hardware, flip this to 1 to additionally suspend the ESP32 itself (instead of
// just busy-looping every 100ms) while the keychain is sitting idle/in a pocket.
// Wakes on a BOOT button press only (no IMU anymore to shake-wake with).
#define USE_LIGHT_SLEEP 0

// =========================================================================
//  👇 STEP 1: DEFINE YOUR IMAGE DIMENSIONS 👇
//  This MUST match the actual pixel dimensions of your sword0.jpg file - it
//  does not upscale it. Bumping this number alone won't add visual detail;
//  you need source art rendered at (or scaled up to) this resolution.
// =========================================================================
#define SWORD_WIDTH   102   
#define SWORD_HEIGHT  249  

// =========================================================================
//  👇 STEP 2: LIST YOUR SWORD IMAGE PATHS HERE 👇
// =========================================================================
const char* swordImages[] = {
  "/sword0.jpg"
  // Only one image in use for now by request. To bring back multiple
  // rotating images later: add more lines here (e.g. "/sword1.jpg", etc.)
  // and uncomment the selectRandomSword() function and its two call sites
  // below (search "RANDOM SWORD SELECTION" - kept intact, just disabled).
};
const int NUM_SWORDS = sizeof(swordImages) / sizeof(swordImages[0]);
int currentSwordIdx = 0; // Tracks which sword is currently active

// --- CACHED SWORD SPRITES (each of the 5 images decoded from JPEG once, not every frame) ---
TFT_eSprite* swordSprites[NUM_SWORDS];     // populated in setup()
TFT_eSprite* jpegTarget = &sprite;         // which sprite the JPEG decoder callback writes into

// --- Minimal QMI8658 I2C driver (shake-to-wake only) ---
// Registers per the QST QMI8658A/C datasheet. We only use the accelerometer.
bool imuReady = false;
uint8_t qmiAddr = 0x6B; // resolved to whichever address ACKs in qmiBegin()
#define QMI8658_REG_WHO_AM_I  0x00
#define QMI8658_WHO_AM_I_VAL  0x05
#define QMI8658_REG_CTRL1     0x02
#define QMI8658_REG_CTRL2     0x03
#define QMI8658_REG_CTRL7     0x08
#define QMI8658_REG_AX_L      0x35

bool qmiWriteReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(qmiAddr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

bool qmiReadRegs(uint8_t reg, uint8_t* buf, uint8_t len) {
  Wire.beginTransmission(qmiAddr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false; // repeated start, keep bus held
  if (Wire.requestFrom((int)qmiAddr, (int)len) != len) return false;
  for (uint8_t i = 0; i < len; i++) buf[i] = Wire.read();
  return true;
}

bool qmiBegin() {
  uint8_t candidates[2] = {0x6B, 0x6A}; // the two common QMI8658 addresses
  for (uint8_t i = 0; i < 2; i++) {
    qmiAddr = candidates[i];
    uint8_t whoami = 0;
    if (qmiReadRegs(QMI8658_REG_WHO_AM_I, &whoami, 1) && whoami == QMI8658_WHO_AM_I_VAL) {
      qmiWriteReg(QMI8658_REG_CTRL1, 0x60); // address auto-increment on burst reads
      qmiWriteReg(QMI8658_REG_CTRL2, 0x13); // accel: +/-4g range, 1000Hz output rate
      qmiWriteReg(QMI8658_REG_CTRL7, 0x01); // enable accelerometer only (gyro axis unused)
      return true;
    }
  }
  return false;
}

bool qmiGetAccel(float &ax, float &ay, float &az) {
  uint8_t raw[6];
  if (!qmiReadRegs(QMI8658_REG_AX_L, raw, 6)) return false;
  int16_t rawX = (int16_t)((raw[1] << 8) | raw[0]);
  int16_t rawY = (int16_t)((raw[3] << 8) | raw[2]);
  int16_t rawZ = (int16_t)((raw[5] << 8) | raw[4]);
  const float sensitivity = 8192.0; // LSB per g at +/-4g full scale
  ax = rawX / sensitivity;
  ay = rawY / sensitivity;
  az = rawZ / sensitivity;
  return true;
}

// --- SHAKE-TO-WAKE ---
// Only sampled while isSleeping (see loop()) - zero cost while the animation
// is actually running. Compares consecutive readings across all 3 axes;
// a deliberate shake produces a much larger delta than gentle handling or
// picking the keychain up, which is the point - this shouldn't wake on every
// touch, only an actual "shake it like a drink" gesture.
float prevAccelX = 0, prevAccelY = 0, prevAccelZ = 0;
#define SHAKE_THRESHOLD 3.0       // was 1.0 - way too sensitive, triggered on a finger touch
#define SHAKE_REQUIRED_SAMPLES 3  // must exceed the threshold this many consecutive checks (~300ms of real shaking) - filters out a single tap/vibration spike
int shakeSampleCount = 0;

// --- MOTION DETECTION & BATTERY SAVER ---
unsigned long lastActivityTime = 0;
bool isSleeping = false;

// --- BUTTON DEBOUNCE ---
bool lastButtonState = HIGH;    // raw reading from the previous loop, used to detect bounce
bool stableButtonState = HIGH;  // debounced value - only changes once a reading has held for 50ms
unsigned long lastDebounceTime = 0;
unsigned long buttonPressStartTime = 0; // when the current debounced press began
bool longPressHandled = false;          // prevents a long-press from ALSO firing as a short-press on release
#define LONG_PRESS_MS 1500              // hold BOOT this long to power off

// Double-tap BOOT toggles the charging screen manually - there's no reliable
// way to auto-detect USB power on this board (checked; no exposed
// charge-status GPIO, and the battery ADC doesn't shift meaningfully on USB
// either). A single short press still just wakes the display as before.
bool showChargingScreen = false;
unsigned long lastReleaseTime = 0;
bool waitingForSecondTap = false;
#define DOUBLE_TAP_WINDOW_MS 400

// --- 3D WEATHER PARTICLES (rain / snow / leaves - same array, different look) ---
#define NUM_DROPS 70
struct Raindrop {
  float x, y, speed, length;
  uint8_t layer;
  uint16_t color; // only used by leaves - assigned when a leaf particle resets to the top
} drops[NUM_DROPS];

// --- RANDOMIZED PARTICLE INTENSITY ---
// Periodically (not every frame) rolls a new speed/density combo so the
// weather doesn't look static forever. The re-roll itself is cheap either
// way; this is really about not wanting the pattern to visibly shift too
// often. Applies to whichever weather type is currently active.
int activeDropCount;                 // how many of the NUM_DROPS array entries are currently falling
float rainSpeedMultiplier = 1.0;
unsigned long lastRainVariation = 0;
#define RAIN_VARIATION_INTERVAL_MS 30000 // re-roll every 30s

void randomizeRainIntensity() {
  activeDropCount = random(NUM_DROPS * 4 / 10, NUM_DROPS + 1); // 40%-100% density
  rainSpeedMultiplier = random(60, 181) / 100.0;                // 0.6x-1.8x speed
}

// --- WEATHER TYPE ROTATION ---
// Every couple of minutes, switches between rain/snow/leaves so the screen
// doesn't look the same forever. Longer interval than the intensity re-roll
// above since changing the whole "season" too often would feel jarring.
enum WeatherType { WEATHER_RAIN, WEATHER_SNOW, WEATHER_LEAVES, WEATHER_SPRING, WEATHER_TYPE_COUNT };
WeatherType currentWeather = WEATHER_RAIN;
unsigned long lastWeatherChange = 0;
#define WEATHER_CHANGE_INTERVAL_MS (2UL * 60UL * 1000UL) // 2 minutes
uint16_t leafColors[3]; // computed once in setup() (needs sprite.color565())
uint16_t petalColors[3]; // same idea, for spring's falling pink petals

void randomizeWeather() {
  currentWeather = (WeatherType)random(0, WEATHER_TYPE_COUNT);
}

// --- OCCASIONAL LIGHTNING ---
// Strikes at an irregular random interval (not a fixed period), briefly
// tinting the background and drawing a jagged procedural bolt. No image
// asset - just a random-walk zigzag of line segments.
unsigned long nextLightningTime = 0;
unsigned long lightningFlashUntil = 0;
int lightningBoltX = 0;

void checkLightning(unsigned long now) {
  if (nextLightningTime == 0) {
    nextLightningTime = now + random(15000, 45000); // first strike 15-45s after boot
    return;
  }
  if (lightningFlashUntil == 0 && now >= nextLightningTime) {
    lightningFlashUntil = now + random(100, 180);        // flash lasts ~100-180ms
    lightningBoltX = random(40, SCREEN_WIDTH - 40);
    nextLightningTime = now + random(20000, 60000);      // schedule the next one, irregular timing
  }
  if (lightningFlashUntil != 0 && now > lightningFlashUntil) {
    lightningFlashUntil = 0; // flash over
  }
}

void drawLightningBoltStrike(int startX, uint16_t color) {
  int x = startX, y = 0;
  const int segments = 9;
  int segHeight = (SCREEN_HEIGHT / 2) / segments;
  for (int i = 0; i < segments; i++) {
    int nextX = constrain(x + random(-14, 15), 10, SCREEN_WIDTH - 10);
    int nextY = y + segHeight;
    sprite.drawLine(x, y, nextX, nextY, color);
    sprite.drawLine(x + 1, y, nextX + 1, nextY, color); // slight thickness
    x = nextX; y = nextY;
  }
}

// --- TEXT CAROUSEL & TYPEWRITER ---
const char* quotes[] = {
  "Steel remembers what flesh forgets.",
  "A blade forged in fire, tempered by will.",
  "Only the brave draw steel.",
  "Honor guides the hand that wields it."
};
char wrappedQuoteBuf[300]; // holds the current quote, re-wrapped to fit the screen (see prepareWrappedQuote)
const int NUM_QUOTES = sizeof(quotes) / sizeof(quotes[0]); // auto-sized, same pattern as NUM_SWORDS
int currentQuoteIdx = 0;
enum TextState { STATE_TYPING, STATE_PAUSE, STATE_ERASING };
TextState textState = STATE_TYPING;
int currentCharIndex = 0;
unsigned long lastTextAnimTime = 0;

// Re-wraps quotes[idx] into wrappedQuoteBuf, inserting '\n' wherever a line
// would actually overflow the screen at the size the quote is drawn at.
// Uses sprite.textWidth() to measure real pixel width - no more guessing
// character counts by hand, which is what caused text to run off-screen
// after the size increase.
void prepareWrappedQuote(int idx) {
  sprite.setTextSize(3); // must match the size the quote is actually drawn at
  const int maxWidth = SCREEN_WIDTH - 20; // exactly 10px margin each side

  String src = quotes[idx];
  String result, line, word;
  int start = 0;

  while (start <= (int)src.length()) {
    int spaceIdx = src.indexOf(' ', start);
    int end = (spaceIdx == -1) ? src.length() : spaceIdx;
    word = src.substring(start, end);

    String testLine = line.length() ? (line + " " + word) : word;
    if (sprite.textWidth(testLine) > maxWidth && line.length() > 0) {
      result += line + "\n";
      line = word;
    } else {
      line = testLine;
    }

    if (end == (int)src.length()) break;
    start = end + 1;
  }
  if (line.length()) result += line;

  result.toCharArray(wrappedQuoteBuf, sizeof(wrappedQuoteBuf));
}

// TFT_eSPI's drawString() only honours the centering datum for the FIRST
// line of a multi-line string - every line after a '\n' gets drawn flush at
// x=0 instead, regardless of datum. This draws each line separately so every
// line is properly centered on its own.
void drawMultilineCentered(const char* text, int x, int yCenter) {
  const int MAX_LINES = 8;
  String lines[MAX_LINES];
  int lineCount = 0;

  String buf = text;
  int start = 0;
  while (lineCount < MAX_LINES) {
    int nl = buf.indexOf('\n', start);
    if (nl == -1) {
      lines[lineCount++] = buf.substring(start);
      break;
    }
    lines[lineCount++] = buf.substring(start, nl);
    start = nl + 1;
  }

  int lineHeight = sprite.fontHeight() + 6; // a little breathing room between lines
  int topY = yCenter - (lineHeight * lineCount) / 2 + lineHeight / 2;
  for (int i = 0; i < lineCount; i++) {
    sprite.drawString(lines[i], x, topY + i * lineHeight);
  }
}

// --- RANDOM SWORD SELECTION (disabled - single image for now) ---
// Kept intact so re-enabling later, once more images are added back to
// swordImages[] above, is just uncommenting this function and its two call
// sites (search "selectRandomSword()" in setup() and wakeDisplay()).
/*
void selectRandomSword() {
  int nextIdx = random(0, NUM_SWORDS);
  // Ensure we don't pick the exact same sword twice in a row (if you have > 1 sword)
  if (NUM_SWORDS > 1 && nextIdx == currentSwordIdx) {
    nextIdx = (nextIdx + 1) % NUM_SWORDS;
  }
  currentSwordIdx = nextIdx;
  Serial.print("Woke up! Selected image: ");
  Serial.println(swordImages[currentSwordIdx]);
}
*/

// =========================================================================
//  👇 JPG DECODER CALLBACK FUNCTION 👇
// =========================================================================
bool tft_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
  if (!jpegTarget) return false;
  // Prevent a crash if sprite memory failed to allocate
  if (jpegTarget->getPointer() == NULL) return false;
  if (y >= jpegTarget->height() || x >= jpegTarget->width()) return false;

  // Push JPEG pixel block into whichever sprite is the current decode target
  jpegTarget->pushImage(x, y, w, h, bitmap);

  return true;
}

// --- Battery sensing, ported from Waveshare's own adc_bsp.c for this board ---
// ADC1 channel 3 = GPIO4. Reads through a 3:1 voltage divider (hence the x3
// below), matching their verified formula exactly.
static adc_oneshot_unit_handle_t adc1_handle;
static adc_cali_handle_t batteryCaliHandle;
static bool batteryCaliOk = false;

// Rough single-cell LiPo range. Tune these two numbers to your exact battery
// if the percentage doesn't feel right in practice - this is a simple linear
// approximation, not a proper discharge-curve lookup.
#define BATTERY_EMPTY_V 3.0
#define BATTERY_FULL_V  4.2

void initBatteryADC() {
  adc_oneshot_unit_init_cfg_t initConfig = { .unit_id = ADC_UNIT_1 };
  ESP_ERROR_CHECK_WITHOUT_ABORT(adc_oneshot_new_unit(&initConfig, &adc1_handle));

  adc_oneshot_chan_cfg_t chanConfig = {
    .atten = ADC_ATTEN_DB_12,
    .bitwidth = ADC_BITWIDTH_12,
  };
  ESP_ERROR_CHECK_WITHOUT_ABORT(adc_oneshot_config_channel(adc1_handle, ADC_CHANNEL_3, &chanConfig));

  adc_cali_curve_fitting_config_t caliConfig = {
    .unit_id = ADC_UNIT_1,
    .atten = ADC_ATTEN_DB_12,
    .bitwidth = ADC_BITWIDTH_12,
  };
  batteryCaliOk = (adc_cali_create_scheme_curve_fitting(&caliConfig, &batteryCaliHandle) == ESP_OK);
  if (!batteryCaliOk) {
    Serial.println("⚠️  ADC calibration unavailable, falling back to uncalibrated battery reading.");
  }
}

float readBatteryVoltage() {
  int raw = 0;
  if (adc_oneshot_read(adc1_handle, ADC_CHANNEL_3, &raw) != ESP_OK) return 0.0;

  if (batteryCaliOk) {
    int millivolts = 0;
    adc_cali_raw_to_voltage(batteryCaliHandle, raw, &millivolts);
    return (millivolts / 1000.0) * 3.0; // x3 for the onboard divider
  }
  return ((float)raw * 3.3 / 4096.0) * 3.0; // uncalibrated fallback, same formula Waveshare uses
}

int getBatteryPercentage() {
  float v = readBatteryVoltage();
  int pct = (int)((v - BATTERY_EMPTY_V) / (BATTERY_FULL_V - BATTERY_EMPTY_V) * 100.0);
  return constrain(pct, 0, 100);
}

// Battery only needs checking occasionally, not 30x/second - this caches the
// last reading and only actually touches the ADC every BATTERY_CHECK_INTERVAL_MS.
#define BATTERY_CHECK_INTERVAL_MS (3UL * 60UL * 1000UL) // 3 minutes
int cachedBatteryPct = 100;
unsigned long lastBatteryCheck = 0;
void updateBatteryIfDue(unsigned long now) {
  if (lastBatteryCheck == 0 || now - lastBatteryCheck >= BATTERY_CHECK_INTERVAL_MS) {
    cachedBatteryPct = getBatteryPercentage();
    lastBatteryCheck = now;
  }
}

// --- PERFORMANCE PROFILER ---
// Times each major stage of the loop (in microseconds) and prints an
// averaged per-frame breakdown to Serial every 3 seconds. "house" covers
// button handling + sleep timeout check + rain re-roll check (everything
// between the frame-rate gate and the actual render starting) - this was
// previously invisible, and is the top suspect since the render stages
// alone don't explain the fps drops we're seeing. "print" is this report
// itself, measured after it prints and shown on the NEXT line (can't measure
// its own cost before printing it).
unsigned long profHouseUs = 0, profFillUs = 0, profRainUs = 0, profSwordUs = 0, profTextUs = 0, profPushUs = 0, profPrintUs = 0;
int profFrameCount = 0;
unsigned long profWindowStart = 0;

void profAdd(unsigned long* accumulator, unsigned long startMicros) {
  *accumulator += micros() - startMicros;
}

void printProfileIfDue(unsigned long now) {
  if (profWindowStart == 0) { profWindowStart = now; return; } // first call just starts the window
  if (now - profWindowStart < 3000) return;

  unsigned long printStart = micros();

  if (profFrameCount > 0) {
    float ms = 1000.0; // divide the us accumulators by this * frame count to get avg ms/frame
    Serial.printf(
      "Frame avg (ms): house=%.2f fill=%.2f rain=%.2f sword=%.2f text=%.2f push=%.2f print=%.2f | total~%.2f | %d frames in %lums (~%.1f fps)\n",
      profHouseUs / ms / profFrameCount,
      profFillUs / ms / profFrameCount,
      profRainUs / ms / profFrameCount,
      profSwordUs / ms / profFrameCount,
      profTextUs / ms / profFrameCount,
      profPushUs / ms / profFrameCount,
      profPrintUs / ms / profFrameCount, // cost of the PREVIOUS report's print, amortized over this window
      (profHouseUs + profFillUs + profRainUs + profSwordUs + profTextUs + profPushUs) / ms / profFrameCount,
      profFrameCount, now - profWindowStart,
      profFrameCount / ((now - profWindowStart) / 1000.0)
    );
  }
  profHouseUs = profFillUs = profRainUs = profSwordUs = profTextUs = profPushUs = 0;
  profFrameCount = 0;
  profWindowStart = now;

  profPrintUs = micros() - printStart; // charged to next window's report
}

void drawMasterSwordImage(int x, int y) {
  if (!swordSprites[currentSwordIdx]) return; // safety: sprite wasn't created (see setup())

  int drawX = x - (SWORD_WIDTH / 2);
  int drawY = y - (SWORD_HEIGHT / 2);

  // Blit the already-decoded sprite for the active sword (decoded once in
  // setup(), see below) instead of re-decoding a JPEG from flash every frame.
  // TFT_BLACK is treated as transparent so the rain/background shows through.
  swordSprites[currentSwordIdx]->pushToSprite(&sprite, drawX, drawY, TFT_BLACK);
}

// Renamed from the old rain-only version - same falling-particle system,
// but now draws differently depending on currentWeather.
void updateAndDrawParticle(int i) {
  float fallSpeed = drops[i].speed * rainSpeedMultiplier;
  float swayAmount = 0;

  if (currentWeather == WEATHER_SNOW) {
    fallSpeed *= 0.35;   // snow drifts down slowly
    swayAmount = sin((millis() + i * 137) / 500.0) * 6.0; // gentle side-to-side drift
  } else if (currentWeather == WEATHER_LEAVES) {
    fallSpeed *= 0.28;   // leaves fall slowest, fluttering down
    swayAmount = sin((millis() + i * 211) / 300.0) * 12.0; // wider flutter than snow
  } else if (currentWeather == WEATHER_SPRING) {
    fallSpeed *= 0.22;   // petals float down slower still, lighter than leaves
    swayAmount = sin((millis() + i * 179) / 280.0) * 10.0;
  }

  drops[i].y += fallSpeed;
  if (drops[i].y > SCREEN_HEIGHT + 20) {
    drops[i].y = -20;
    drops[i].x = random(-50, SCREEN_WIDTH + 50);
    if (currentWeather == WEATHER_LEAVES) {
      drops[i].color = leafColors[random(0, 3)]; // new random autumn shade each time it resets
    } else if (currentWeather == WEATHER_SPRING) {
      drops[i].color = petalColors[random(0, 3)]; // new random pink shade each time it resets
    }
  }

  int drawX = drops[i].x + swayAmount;
  int drawY = drops[i].y;
  int size = (drops[i].layer == 2) ? 2 : 1; // foreground particles slightly bigger, same depth cue as before

  if (currentWeather == WEATHER_RAIN) {
    sprite.drawLine(drawX, drawY, drawX, drawY + drops[i].length, TFT_WHITE);
    if (drops[i].layer == 2) {
      sprite.drawLine(drawX + 1, drawY, drawX + 1, drawY + drops[i].length, TFT_WHITE);
    }
  } else if (currentWeather == WEATHER_SNOW) {
    sprite.fillCircle(drawX, drawY, size, TFT_WHITE);
  } else { // WEATHER_LEAVES or WEATHER_SPRING - both just colored dots, palette differs
    sprite.fillCircle(drawX, drawY, size, drops[i].color);
  }
}

// Procedural lightning bolt (two overlapping triangles forming a zigzag) -
// no image asset needed. Proportions are a first pass; easy to nudge once
// you've actually seen it rendered.
void drawLightningBolt(int cx, int cy, float size, uint16_t color) {
  sprite.fillTriangle(
    cx + size * 0.25, cy - size,
    cx + size * 0.45, cy - size * 0.05,
    cx - size * 0.05, cy + size * 0.05,
    color);
  sprite.fillTriangle(
    cx + size * 0.05, cy - size * 0.05,
    cx - size * 0.45, cy + size * 0.05,
    cx - size * 0.25, cy + size,
    color);
}

// Charging screen (double-tap BOOT to toggle on/off): a pulsing gold ring
// with a rotating "energy" sweep and a lightning bolt at center, rain still
// falling behind it for atmosphere. Entirely procedural/vector-drawn, no new
// image assets. Purely time-based animation - doesn't track charge % at all.
void renderChargingScreen(unsigned long now) {
  sprite.fillSprite(TFT_BLACK);

  for (int i = 0; i < activeDropCount; i++) {
    if (drops[i].layer == 0 || drops[i].layer == 1) updateAndDrawParticle(i);
  }

  int cx = SCREEN_WIDTH / 2;
  int cy = SCREEN_HEIGHT / 2;
  int ringRadius = 70;

  // Slow "breathing" glow rather than a flat static color.
  float pulse = (sin(now / 400.0) + 1.0) / 2.0; // 0..1
  uint8_t glowG = 150 + (uint8_t)(pulse * 90);   // 150-240
  uint16_t ringColor = sprite.color565(255, glowG, 0);

  // Dim full-circle track, then a rotating highlighted arc sweeping around it
  // (like energy flowing into the sword) instead of a percentage-based fill.
  int sweepStart = (now / 4) % 360;   // one full rotation roughly every 1.4s
  int sweepLength = 100;              // degrees of the bright arc
  sprite.drawArc(cx, cy, ringRadius, ringRadius - 10, 0, 360, sprite.color565(45, 36, 0), TFT_BLACK, false);
  sprite.drawArc(cx, cy, ringRadius, ringRadius - 10, sweepStart, sweepStart + sweepLength, ringColor, TFT_BLACK, true);

  drawLightningBolt(cx, cy, 32, ringColor);

  for (int i = 0; i < activeDropCount; i++) {
    if (drops[i].layer == 2) updateAndDrawParticle(i);
  }

  sprite.setTextDatum(MC_DATUM);
  sprite.setTextColor(TFT_GOLD, TFT_BLACK);
  sprite.setTextSize(2);
  sprite.drawString("CHARGING", cx, cy + ringRadius + 35);
  sprite.setTextSize(1);

  esp_lcd_panel_draw_bitmap(lcd_panel_handle, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, sprite.getPointer());
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  randomSeed(micros());

  // 1. Initialize the REAL display panel driver (esp_lcd + SH8601, matches
  // Waveshare's own working example for this exact board/panel). This
  // replaces the old TFT_eSPI hardware init, which was never built for this
  // panel's QSPI interface and is the root cause of the crashes we were
  // chasing - TFT_eSPI is now used purely as an in-memory 2D drawing toolkit
  // below, with no real hardware attached to it.
  initDisplay();

  // Battery ADC - see initBatteryADC() for the real (not hardcoded) reading
  initBatteryADC();

  // IMU - shake-to-wake only, see the driver block above for scope notes.
  Wire.begin(I2C_SDA, I2C_SCL);
  imuReady = qmiBegin();
  if (imuReady) {
    Serial.println("✅ QMI8658 IMU ready (shake-to-wake only).");
  } else {
    Serial.println("⚠️  QMI8658 IMU not found - shake-to-wake disabled, button-only wake remains.");
  }

  // 2. TFT_eSPI is used ONLY for its software sprite/drawing functions now

  // 3. Enable PSRAM Sprite Buffer
  sprite.setColorDepth(16);
  
  // Attempt to create sprite buffer in PSRAM
  void* ptr = sprite.createSprite(SCREEN_WIDTH, SCREEN_HEIGHT);
  if (ptr == NULL) {
    Serial.println("❌ CRITICAL ERROR: Could not allocate Sprite memory!");
    Serial.println("   Please check Tools -> PSRAM is set to 'OPI PSRAM'.");
    while(1) { delay(1000); } // Halt safely instead of crashing
  } else {
    Serial.println("✅ Sprite buffer created successfully in PSRAM!");
  }

  // 4. Initialize Storage
  if (!LittleFS.begin(true)) {
    Serial.println("❌ CRITICAL ERROR: LittleFS Mount Failed!");
    Serial.println("   Check Tools -> Partition Scheme includes a LittleFS/SPIFFS partition.");
    while(1) { delay(1000); } // Halt safely - continuing past this point with no
                               // filesystem left swordSprites[] uninitialized and
                               // crashed on first use, which is what caused the boot loop.
  }

  // 5. Setup JPEG Decoder
  TJpgDec.setJpgScale(1);           
  TJpgDec.setSwapBytes(true);       
  TJpgDec.setCallback(tft_output);  

  // 5b. Decode the sword image(s) ONE TIME into their own small cached sprites.
  // Each allocation is checked so a failure halts safely with a clear
  // message instead of crashing.
  for (int i = 0; i < NUM_SWORDS; i++) {
    swordSprites[i] = new TFT_eSprite(&tft);
    if (!swordSprites[i]) {
      Serial.println("❌ CRITICAL ERROR: Out of memory creating sword sprite object!");
      while(1) { delay(1000); }
    }
    void* spritePtr = swordSprites[i]->createSprite(SWORD_WIDTH, SWORD_HEIGHT);
    if (spritePtr == NULL) {
      Serial.println("❌ CRITICAL ERROR: Could not allocate sword sprite buffer!");
      Serial.println("   Please check Tools -> PSRAM is set to 'OPI PSRAM'.");
      while(1) { delay(1000); }
    }
    swordSprites[i]->fillSprite(TFT_BLACK);
    jpegTarget = swordSprites[i];
    TJpgDec.drawFsJpg(0, 0, swordImages[i], LittleFS);
  }
  jpegTarget = &sprite; // callback now targets the main frame sprite again

  // selectRandomSword(); // disabled - single image for now, see definition above

  // 6. Setup Raindrops
  for (int i = 0; i < NUM_DROPS; i++) {
    drops[i].x = random(-50, SCREEN_WIDTH + 50);
    drops[i].y = random(-50, SCREEN_HEIGHT + 50);
    drops[i].layer = random(0, 3);
    // Faster, shorter droplets than before (was 0.8-3.8 px/frame and up to
    // 26px long - both too slow and too "streaky" looking).
    if (drops[i].layer == 0) { drops[i].speed = random(30, 45)/10.0; drops[i].length = random(3, 6); }
    else if (drops[i].layer == 1) { drops[i].speed = random(50, 70)/10.0; drops[i].length = random(4, 7); }
    else { drops[i].speed = random(80, 110)/10.0; drops[i].length = random(5, 9); }
  }

  randomizeRainIntensity(); // initial speed/density before the first 30s re-roll
  randomizeWeather();       // initial weather type before the first 2min re-roll
  leafColors[0] = sprite.color565(200, 100, 20);  // burnt orange
  leafColors[1] = sprite.color565(180, 60, 20);   // rust red
  leafColors[2] = sprite.color565(220, 160, 30);  // gold
  petalColors[0] = sprite.color565(255, 182, 210); // soft pink
  petalColors[1] = sprite.color565(255, 140, 190); // hot pink
  petalColors[2] = sprite.color565(255, 220, 230); // pale blush

  prepareWrappedQuote(currentQuoteIdx); // wrap the first quote before loop() needs it

  lastActivityTime = millis();
}

// --- WAKE / SLEEP HELPERS ---
// Centralised so the CPU-clock scaling + real panel sleep-in/out commands
// only need to be written once instead of duplicated at every wake site.
void wakeDisplay() {
  isSleeping = false;
  setCpuFrequencyMhz(CPU_MHZ_ACTIVE);
  // selectRandomSword(); // disabled - single image for now, see definition above
  lcdSendCmd(0x11);          // Sleep OUT
  delay(120);                // panel needs ~120ms before it'll accept further commands
  esp_lcd_panel_disp_on_off(lcd_panel_handle, true); // Display ON
}

void enterSleep() {
  isSleeping = true;
  sprite.fillSprite(TFT_BLACK);
  esp_lcd_panel_draw_bitmap(lcd_panel_handle, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, sprite.getPointer());
  esp_lcd_panel_disp_on_off(lcd_panel_handle, false); // Display OFF
  lcdSendCmd(0x10);          // Sleep IN - deeper power-down than Display Off alone
  setCpuFrequencyMhz(CPU_MHZ_IDLE);
}

// Real "off" via ESP32 deep sleep - down to a few microamps, as close to
// truly powered-off as we can get in software on this board. Wakes on the
// next BOOT press, which causes a full reset (setup() runs again from
// scratch - display, LittleFS, everything reinitializes normally).
void powerOff() {
  Serial.println("Powering off (deep sleep). Hold BOOT to turn back on.");

  sprite.fillSprite(TFT_BLACK);
  esp_lcd_panel_draw_bitmap(lcd_panel_handle, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, sprite.getPointer());
  esp_lcd_panel_disp_on_off(lcd_panel_handle, false);
  lcdSendCmd(0x10);
  delay(50);

  // Wait for the button to actually be released before arming the wakeup -
  // otherwise, since it's still held down right now, deep sleep would see
  // that as an already-satisfied wake condition and immediately wake back up.
  while (digitalRead(BUTTON_PIN) == LOW) {
    delay(10);
  }

  esp_sleep_enable_ext0_wakeup((gpio_num_t)BUTTON_PIN, 0); // wake on next LOW (press)
  esp_deep_sleep_start(); // does not return
}

void loop() {
  unsigned long now = millis();
  updateBatteryIfDue(now);

  // Frame-rate cap: an uncapped loop renders as fast as it possibly can.
  // 30fps looks identical for this content and uses a fraction of the CPU time.
  static unsigned long lastFrameTime = 0;
  if (!isSleeping && (now - lastFrameTime) < TARGET_FRAME_MS) {
    delay(1); // brief yield instead of a pure busy-spin
    return;
  }
  lastFrameTime = now;
  unsigned long tHouse = micros();

  // 1. Hardware Button Input (single press = wake, double tap = toggle
  // charging screen, long press = power off)
  int reading = digitalRead(BUTTON_PIN);
  if (reading != lastButtonState) {
    lastDebounceTime = now;
  }
  if ((now - lastDebounceTime) > 50) {
    if (reading != stableButtonState) {
      stableButtonState = reading;
      if (stableButtonState == LOW) {
        // Debounced press just started - just start timing it, don't act yet.
        buttonPressStartTime = now;
        longPressHandled = false;
      } else {
        // Debounced release - only a short-press action if a long-press
        // action hasn't already fired while it was held.
        if (!longPressHandled) {
          if (waitingForSecondTap && (now - lastReleaseTime) < DOUBLE_TAP_WINDOW_MS) {
            // Second tap within the window - double tap confirmed.
            showChargingScreen = !showChargingScreen;
            waitingForSecondTap = false;
            lastActivityTime = now;
            if (isSleeping) wakeDisplay();
          } else {
            // Could be a single tap, or the first half of a double tap -
            // wait to see if a second tap follows before deciding.
            waitingForSecondTap = true;
            lastReleaseTime = now;
          }
        }
      }
    }
  }
  // Long-hold check happens continuously WHILE held, so it fires without
  // waiting for release.
  if (stableButtonState == LOW && !longPressHandled && (now - buttonPressStartTime) > LONG_PRESS_MS) {
    longPressHandled = true;
    powerOff(); // does not return
  }
  // If we were waiting for a second tap and the window passed with no second
  // tap arriving, it was just a single press - do the normal wake action now.
  if (waitingForSecondTap && (now - lastReleaseTime) >= DOUBLE_TAP_WINDOW_MS) {
    waitingForSecondTap = false;
    lastActivityTime = now;
    if (isSleeping) wakeDisplay();
  }
  lastButtonState = reading;

  // Put device to sleep after inactivity timeout. Wake is button-only now
  // (no IMU/motion detection - removed entirely, see top of file).
  if (now - lastActivityTime > INACTIVITY_TIMEOUT_MS && !isSleeping && !showChargingScreen) {
    enterSleep();
  }

  if (isSleeping) {
    bool shouldWake = false;

    // Shake-to-wake check - only runs while asleep, so this has zero cost
    // during normal operation.
    if (!shouldWake && imuReady) {
      float ax = 0, ay = 0, az = 0;
      if (qmiGetAccel(ax, ay, az)) {
        float delta = fabs(ax - prevAccelX) + fabs(ay - prevAccelY) + fabs(az - prevAccelZ);
        prevAccelX = ax; prevAccelY = ay; prevAccelZ = az;
        if (delta > SHAKE_THRESHOLD) {
          shakeSampleCount++;
          if (shakeSampleCount >= SHAKE_REQUIRED_SAMPLES) {
            shouldWake = true;
            shakeSampleCount = 0;
          }
        } else {
          shakeSampleCount = 0; // streak broken, a real shake needs to be sustained
        }
      }
    }

    if (shouldWake) {
      lastActivityTime = now;
      wakeDisplay();
      // Deliberately no "return" here - isSleeping is now false, so
      // execution falls through and renders immediately this same frame,
      // same as a button-triggered wake does.
    } else {
#if USE_LIGHT_SLEEP
      // True MCU sleep: suspends the ESP32 core itself between checks
      // instead of just looping with a delay(). NOTE: shake-to-wake doesn't
      // work in this mode as written - light sleep halts the CPU entirely,
      // so nothing is polling the IMU. Only the BOOT button will wake it
      // here. millis() keeps ticking through light sleep so all the timing
      // logic above continues to work correctly on wake.
      esp_sleep_enable_ext0_wakeup((gpio_num_t)BUTTON_PIN, 0);
      esp_light_sleep_start();
#else
      delay(100);
#endif
      return;
    }
  }

  // Rain intensity re-rolls periodically instead of every frame - see
  // randomizeRainIntensity() for what it actually changes.
  if (now - lastRainVariation > RAIN_VARIATION_INTERVAL_MS) {
    lastRainVariation = now;
    randomizeRainIntensity();
  }

  // Weather type rotates on a much longer interval than intensity does.
  if (now - lastWeatherChange > WEATHER_CHANGE_INTERVAL_MS) {
    lastWeatherChange = now;
    randomizeWeather();
  }

  checkLightning(now);
  profAdd(&profHouseUs, tHouse);

  // 2. Render Frame
  if (showChargingScreen) {
    // Dedicated charging screen (double-tap BOOT to toggle) - not run
    // through the stage profiler since it's a different render path
    // entirely; profiler output just pauses while shown and resumes
    // normally once toggled off.
    renderChargingScreen(now);
    return;
  }

  bool lightningFlashing = (lightningFlashUntil != 0 && now < lightningFlashUntil);

  unsigned long tStart = micros();
  // A brief dim blue-grey tint instead of pure black while lightning flashes,
  // like the scene is momentarily lit from outside.
  sprite.fillSprite(lightningFlashing ? sprite.color565(50, 50, 70) : TFT_BLACK);
  profAdd(&profFillUs, tStart);

  tStart = micros();
  // Background weather (rain/snow/leaves depending on currentWeather)
  for (int i = 0; i < activeDropCount; i++) {
    if (drops[i].layer == 0 || drops[i].layer == 1) updateAndDrawParticle(i);
  }
  profAdd(&profRainUs, tStart);

  // Sword Floating Motion - anchored on true screen center, with a gentle
  // idle sway (no more gyro parallax - IMU/gyro removed entirely).
  float idleOffsetY = sin(now / 450.0) * 10.0; 
  int swordX = SCREEN_WIDTH / 2;
  // Clamp range for the 249px-tall image, plus the +40 base offset and idle sway.
  int swordY = constrain((SCREEN_HEIGHT / 2.0) + 40 + idleOffsetY, 128, 328);
  
  tStart = micros();
  drawMasterSwordImage(swordX, swordY);
  if (lightningFlashing) {
    drawLightningBoltStrike(lightningBoltX, TFT_WHITE);
  }
  profAdd(&profSwordUs, tStart);

  tStart = micros();
  // Foreground weather (rain/snow/leaves depending on currentWeather)
  for (int i = 0; i < activeDropCount; i++) {
    if (drops[i].layer == 2) updateAndDrawParticle(i);
  }
  profAdd(&profRainUs, tStart);

  tStart = micros();
  // Typewriter Text
  const char* activeQuote = wrappedQuoteBuf;
  int activeLength = strlen(activeQuote);

  if (textState == STATE_TYPING) {
    if (now - lastTextAnimTime > 80) {
      if (currentCharIndex < activeLength) currentCharIndex++;
      else textState = STATE_PAUSE;
      lastTextAnimTime = now;
    }
  } else if (textState == STATE_PAUSE) {
    if (now - lastTextAnimTime > 10000) {
      textState = STATE_ERASING;
      lastTextAnimTime = now;
    }
  } else if (textState == STATE_ERASING) {
    if (now - lastTextAnimTime > 35) {
      if (currentCharIndex > 0) currentCharIndex--;
      else {
        currentQuoteIdx = (currentQuoteIdx + 1) % NUM_QUOTES;
        prepareWrappedQuote(currentQuoteIdx);
        textState = STATE_TYPING;
      }
      lastTextAnimTime = now;
    }
  }

  char currentTextDisplay[256];
  strncpy(currentTextDisplay, activeQuote, currentCharIndex);
  currentTextDisplay[currentCharIndex] = '\0';

  sprite.setTextDatum(MC_DATUM);
  sprite.setTextColor(TFT_WHITE, TFT_BLACK);
  sprite.setTextSize(3); // was invisible-small at the default size 1
  drawMultilineCentered(currentTextDisplay, SCREEN_WIDTH / 2, 75);

  // Battery Display - moved to the bottom-right (was top-right) to give the
  // quote text the full top area to itself.
  sprite.setTextDatum(BR_DATUM);
  sprite.setTextColor(TFT_GOLD, TFT_BLACK); // was a harsh pure green
  sprite.setTextSize(2); // was the default size 1 (tiny)
  char batStr[8];
  snprintf(batStr, sizeof(batStr), "%d%%", cachedBatteryPct);
  sprite.drawString(batStr, SCREEN_WIDTH - 12, SCREEN_HEIGHT - 12);
  sprite.setTextSize(1); // reset so nothing else on screen inherits this
  profAdd(&profTextUs, tStart);

  // Push the finished frame to the REAL panel driver (TFT_eSPI's own
  // pushSprite() is never used - it would try to talk over the wrong bus).
  tStart = micros();
  esp_lcd_panel_draw_bitmap(lcd_panel_handle, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, sprite.getPointer());
  profAdd(&profPushUs, tStart);

  profFrameCount++;
  printProfileIfDue(now);
}