#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <time.h>
#include <esp_sleep.h>
#include <Wire.h>
#include <math.h>

// ================= DEFAULT USER CONFIGURATION =================
String wifiSSID = "EXT2.4";
String wifiPass = "LAXMI1234";
String cityName = "NEW DELHI";
float latitude = 28.6139;
float longitude = 77.2090;
long gmtOffsetSec = 19800; // Default: India (UTC+5:30)
// ==============================================================

Preferences prefs;
WebServer server(80);
bool apModeActive = false;

// ==================== LILYGO T-EMBED CC1101 PLUS PINS ====================
#define BOARD_PWR_EN   15   // बोर्ड और पेरिफेरल पावर पिन
#define PIN_POWER_ON   BOARD_PWR_EN

// ST7789 1.9" IPS डिस्प्ले पिनें (170x320)
#define DISPLAY_BL     21
#define DISPLAY_SCLK   11
#define DISPLAY_MOSI   9
#define DISPLAY_MISO   10
#define DISPLAY_DC     16
#define DISPLAY_RST    -1
#define DISPLAY_CS     41

// साझा SPI बस की चिप-सेलेक्ट पिनें (इन्हें HIGH रखना ज़रूरी है)
#define SD_CS          13
#define CC1101_CS      12
#define PIN_NRF24_CS   45

// बटन, रोटरी एन्कोडर और I2C बैटरी गेज पिनें
#define PIN_ENCODER_BTN 0
#define PIN_ENCODER_A   4
#define PIN_ENCODER_B   5
#define PIN_BACK_BTN    6
#define PIN_BAT_ADC     4
#define PIN_I2C_SDA     8
#define PIN_I2C_SCL     18

Adafruit_ST7789 tft = Adafruit_ST7789(&SPI, DISPLAY_CS, DISPLAY_DC, DISPLAY_RST);
GFXcanvas16 *spr = nullptr;

// Text Alignment Helper Enum
enum TextDatum { TL_DATUM, TR_DATUM, MC_DATUM, MR_DATUM };

// Colour Palette (RGB565)
#define BG_DARK      0x0000 // Deep Midnight Navy
#define CARD_NAVY    0x0000 // Rich Slate Blue
#define CARD_PURPLE  0x0000 // Deep Indigo
#define CARD_TEAL    0x0000 // Deep Ocean Teal
#define ACCENT_CYAN  0x07FF // Electric Cyan
#define ACCENT_GOLD  0xFEA0 // Warm Sun Gold
#define ACCENT_CORAL 0xFA68 // Vibrant Coral
#define ACCENT_MINT  0x27F4 // Fresh Mint Green
#define TEXT_WHITE   0xFFFF
#define TEXT_MUTED   0x9CD3

// Weather Data Structure
struct DayForecast {
  String dayName;
  int code;
  float maxTemp;
  float minTemp;
};

struct WeatherData {
  float currentTemp = 0.0;
  float feelsLike = 0.0;
  int humidity = 0;
  float windSpeed = 0.0;
  int weatherCode = 0;
  bool valid = false;
  DayForecast days[7];
} weather;

// Screen Cycling State
enum ScreenMode { SCREEN_TIME = 0, SCREEN_CURRENT = 1, SCREEN_FORECAST = 2 };
int currentScreen = SCREEN_TIME;
bool showIpScreen = false;
bool autoSleepEnabled = true;
bool autoCycleEnabled = true;
unsigned long lastCycleMs = 0;
unsigned long lastWeatherMs = 0;
unsigned long lastActivityMs = 0;
bool isDisplayAsleep = false;

// Smart Refresh Tracking
bool needsRedraw = true;
int lastDrawnSec = -1;
bool wasHudActive = false;

const unsigned long CYCLE_INTERVAL  = 5000;   // 5 seconds per screen
const unsigned long WEATHER_REFRESH = 900000; // Refresh weather every 15 mins
const unsigned long LONG_PRESS_MS   = 1500;   // 1.5 seconds long press
const unsigned long SCREEN_TIMEOUT  = 30000;  // 30 seconds display timeout

// Brightness & Encoder State
volatile int encoderDelta = 0;
volatile uint8_t lastEncState = 0;
int brightness = 200; // Initial PWM brightness (15 - 255)
unsigned long lastBrightnessMs = 0;
const unsigned long HUD_TIMEOUT = 1500; // Show brightness bar for 1.5s

// Button State Tracking
unsigned long backPressStartMs = 0;
bool backBtnPressed = false;
unsigned long encPressStartMs = 0;
bool encBtnPressed = false;
bool encLongPressHandled = false;
bool wokeFromPress = false;

// Forward Declarations
void loadSettings();
void setupWebServer();
void handleRoot();
void handleSave();
bool geocodeCity(const String& queryCity, float &outLat, float &outLon, String &outResolvedName);
String urlEncode(const String& str);
void fetchWeather();
void drawTimeScreen();
void drawCurrentWeatherScreen();
void drawForecastScreen();
void drawIpScreen();
void drawQRCode(int startX, int startY, const char* text, int scale);
void drawWeatherIcon(int cx, int cy, int code, int scale);
String getWeatherDesc(int code);
String getShortDayName(const char* dateStr);
void drawProgressDots(int activeIdx);
void setBacklight(int val);
void drawBrightnessOverlay();
void powerOffDevice();
int getBatteryPercentage();
void pushFrame();
void drawAlignedText(const String& text, int x, int y, uint8_t size, uint16_t color, uint16_t bg, TextDatum datum, int maxWidth = 0);

// Push full off-screen canvas to ST7789 display
void pushFrame() {
  if (spr && spr->getBuffer()) {
    tft.drawRGBBitmap(0, 0, spr->getBuffer(), 320, 170);
  }
}

// Helper for aligned text rendering on GFXcanvas16
void drawAlignedText(const String& text, int x, int y, uint8_t size, uint16_t color, uint16_t bg, TextDatum datum, int maxWidth) {
  spr->setTextWrap(false);
  spr->setTextColor(color, bg);
  uint8_t s = size;
  int16_t x1, y1;
  uint16_t w, h;
  spr->setTextSize(s);
  spr->getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  while (maxWidth > 0 && w > (uint16_t)maxWidth && s > 1) {
    s--;
    spr->setTextSize(s);
    spr->getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  }

  int drawX = x;
  int drawY = y;
  switch (datum) {
    case TL_DATUM:
      drawX = x;
      drawY = y;
      break;
    case TR_DATUM:
      drawX = x - w;
      drawY = y;
      break;
    case MC_DATUM:
      drawX = x - (w / 2);
      drawY = y - (h / 2);
      break;
    case MR_DATUM:
      drawX = x - w;
      drawY = y - (h / 2);
      break;
  }
  spr->setCursor(drawX, drawY);
  spr->print(text);
}

// Read Battery Percentage (BQ27220 I2C Fuel Gauge with ADC fallback)
int getBatteryPercentage() {
  Wire.beginTransmission(0x55);
  Wire.write(0x2C); // State of Charge (SOC) register on BQ27220
  if (Wire.endTransmission(false) == 0 && Wire.requestFrom(0x55, 2) == 2) {
    uint16_t soc = Wire.read() | (Wire.read() << 8);
    if (soc <= 100) return soc;
  }
  uint32_t mv = analogReadMilliVolts(PIN_BAT_ADC) * 2;
  if (mv < 2500) return 100;
  return constrain(map(mv, 3300, 4200, 0, 100), 0, 100);
}

// Quadrature Rotary Encoder ISR
void IRAM_ATTR readEncoderISR() {
  uint8_t a = digitalRead(PIN_ENCODER_A);
  uint8_t b = digitalRead(PIN_ENCODER_B);
  uint8_t state = (a << 1) | b;
  if (state != lastEncState) {
    if ((lastEncState == 0b00 && state == 0b01) ||
        (lastEncState == 0b01 && state == 0b11) ||
        (lastEncState == 0b11 && state == 0b10) ||
        (lastEncState == 0b10 && state == 0b00)) {
      encoderDelta++;
    } else {
      encoderDelta--;
    }
    lastEncState = state;
  }
}

// Helper for ESP32 Arduino Core 2.x & 3.x PWM compatibility
void setBacklight(int val) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(DISPLAY_BL, val);
#else
  ledcWrite(0, val);
#endif
}

// Load saved configuration from NVS Flash
void loadSettings() {
  prefs.begin("weather_cfg", true);
  wifiSSID = prefs.getString("ssid", wifiSSID);
  wifiPass = prefs.getString("pass", wifiPass);
  cityName = prefs.getString("city", cityName);
  latitude = prefs.getFloat("lat", latitude);
  longitude = prefs.getFloat("lon", longitude);
  gmtOffsetSec = prefs.getLong("gmt", gmtOffsetSec);
  autoSleepEnabled = prefs.getBool("autosleep", autoSleepEnabled);
  autoCycleEnabled = prefs.getBool("autocycle", autoCycleEnabled);
  prefs.end();
}

void setup() {
  Serial.begin(115200);

  // Enable board peripheral power rail
  pinMode(BOARD_PWR_EN, OUTPUT);
  digitalWrite(BOARD_PWR_EN, HIGH);
  delay(150);

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);

  // Isolate shared SPI devices BEFORE SPI.begin
  pinMode(DISPLAY_CS, OUTPUT);   digitalWrite(DISPLAY_CS, HIGH);
  pinMode(SD_CS, OUTPUT);        digitalWrite(SD_CS, HIGH);
  pinMode(CC1101_CS, OUTPUT);    digitalWrite(CC1101_CS, HIGH);
  pinMode(PIN_NRF24_CS, OUTPUT); digitalWrite(PIN_NRF24_CS, HIGH);

  // Buttons & Rotary Encoder Inputs
  pinMode(PIN_ENCODER_BTN, INPUT_PULLUP);
  pinMode(PIN_BACK_BTN, INPUT_PULLUP);
  pinMode(PIN_ENCODER_A, INPUT_PULLUP);
  pinMode(PIN_ENCODER_B, INPUT_PULLUP);

  lastEncState = (digitalRead(PIN_ENCODER_A) << 1) | digitalRead(PIN_ENCODER_B);
  attachInterrupt(digitalPinToInterrupt(PIN_ENCODER_A), readEncoderISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_ENCODER_B), readEncoderISR, CHANGE);

  loadSettings();

  // Initialize Shared SPI & ST7789 1.9" IPS Display (170x320)
  SPI.begin(DISPLAY_SCLK, DISPLAY_MISO, DISPLAY_MOSI, DISPLAY_CS);
  tft.init(170, 320);
  tft.setSPISpeed(27000000);
  tft.setRotation(1); // Landscape 320x170 (उल्टा दिखे तो 3 कर दें)
  tft.invertDisplay(true);
  tft.fillScreen(BG_DARK);

  // Initialize Backlight PWM (5 kHz, 8-bit resolution)
  pinMode(DISPLAY_BL, OUTPUT);
  digitalWrite(DISPLAY_BL, HIGH);
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(DISPLAY_BL, 5000, 8);
#else
  ledcSetup(0, 5000, 8);
  ledcAttachPin(DISPLAY_BL, 0);
#endif
  setBacklight(brightness);

  // Create full-screen canvas for tear-free rendering (before Wi-Fi starts)
  spr = new GFXcanvas16(320, 170);
  spr->cp437(true); // असली डिग्री चिह्न (°) के लिए CP437 एनेबल करें

  // Boot Splash Screen
  spr->fillScreen(BG_DARK);
  drawAlignedText("LILYGO WEATHER STATION", 160, 65, 2, ACCENT_CYAN, BG_DARK, MC_DATUM);
  drawAlignedText("Connecting to Wi-Fi...", 160, 105, 2, TEXT_MUTED, BG_DARK, MC_DATUM);
  pushFrame();

  // Connect Wi-Fi (Fallback to Hotspot AP if connection fails so user can configure Wi-Fi)
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSSID.c_str(), wifiPass.c_str());
  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 25) {
    delay(500);
    retries++;
  }

  if (WiFi.status() != WL_CONNECTED) {
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP("T-Embed-Setup");
    apModeActive = true;
  }

  // Start Web UI Server
  setupWebServer();

  // Configure Time Zone Offset
  configTime(gmtOffsetSec, 0, "pool.ntp.org", "time.google.com", "in.pool.ntp.org");

  spr->fillScreen(BG_DARK);
  drawAlignedText("Syncing Time & Weather...", 160, 85, 2, ACCENT_MINT, BG_DARK, MC_DATUM, 300);
  pushFrame();

  struct tm timeinfo;
  getLocalTime(&timeinfo, 3000);

  fetchWeather();
  lastWeatherMs = millis();
  lastCycleMs = millis();
  lastActivityMs = millis();
  needsRedraw = true;
}

void loop() {
  unsigned long now = millis();

  // Process incoming Web UI HTTP requests
  server.handleClient();

  // 1. Handle Rotary Encoder Brightness Adjustment & Wake
  noInterrupts();
  int delta = encoderDelta;
  encoderDelta = 0;
  interrupts();

  if (delta != 0) {
    lastActivityMs = now;
    if (isDisplayAsleep) {
      isDisplayAsleep = false;
      setBacklight(brightness);
      lastCycleMs = now;
    } else {
      brightness = constrain(brightness + (delta * 8), 15, 255);
      setBacklight(brightness);
      lastBrightnessMs = now;
    }
    needsRedraw = true;
  }

  // 2. Handle Dedicated Back Button (Wake on press, Short-Press exits IP screen, Long-Press -> Power Off)
  if (digitalRead(PIN_BACK_BTN) == LOW) {
    lastActivityMs = now;
    if (isDisplayAsleep) {
      isDisplayAsleep = false;
      setBacklight(brightness);
      lastCycleMs = now;
      needsRedraw = true;
    }
    if (!backBtnPressed) {
      backBtnPressed = true;
      backPressStartMs = now;
    } else if (now - backPressStartMs >= LONG_PRESS_MS) {
      powerOffDevice();
    }
  } else {
    if (backBtnPressed) {
      if (now - backPressStartMs > 40 && now - backPressStartMs < LONG_PRESS_MS && showIpScreen) {
        showIpScreen = false;
        lastCycleMs = now;
        needsRedraw = true;
      }
      backBtnPressed = false;
    }
  }

  // 3. Handle Encoder Center Button (Short press = Next Screen / Exit IP, Long press = Show Web UI IP Screen)
  if (digitalRead(PIN_ENCODER_BTN) == LOW) {
    lastActivityMs = now;
    if (!encBtnPressed) {
      encBtnPressed = true;
      encLongPressHandled = false;
      encPressStartMs = now;
      if (isDisplayAsleep) {
        isDisplayAsleep = false;
        wokeFromPress = true;
        setBacklight(brightness);
        lastCycleMs = now;
        needsRedraw = true;
      } else {
        wokeFromPress = false;
      }
    } else if (!encLongPressHandled && (now - encPressStartMs >= LONG_PRESS_MS)) {
      encLongPressHandled = true;
      showIpScreen = !showIpScreen;
      lastCycleMs = now;
      needsRedraw = true;
    }
  } else {
    if (encBtnPressed) {
      if (!wokeFromPress && !encLongPressHandled && now - encPressStartMs > 40) {
        if (showIpScreen) {
          showIpScreen = false;
        } else {
          currentScreen = (currentScreen + 1) % 3;
        }
        lastCycleMs = now;
        needsRedraw = true;
      }
      encBtnPressed = false;
      encLongPressHandled = false;
      wokeFromPress = false;
    }
  }

  // 4. Check 30-Second Display Timeout
  if (autoSleepEnabled && !isDisplayAsleep && (now - lastActivityMs >= SCREEN_TIMEOUT)) {
    isDisplayAsleep = true;
    setBacklight(0);
  }

  // 5. Auto-cycle every 5 seconds (Pause cycling while IP Screen is displayed)
  if (autoCycleEnabled && !showIpScreen && (now - lastCycleMs >= CYCLE_INTERVAL)) {
    lastCycleMs = now;
    currentScreen = (currentScreen + 1) % 3;
    needsRedraw = true;
  }

  // 6. Periodic weather update
  if (now - lastWeatherMs >= WEATHER_REFRESH) {
    lastWeatherMs = now;
    if (WiFi.status() == WL_CONNECTED) {
      fetchWeather();
      needsRedraw = true;
    }
  }

  // 7. Smart Render (केवल ज़रूरी होने पर ही SPI फ्रेम भेजें; एनिमेटेड वेदर स्क्रीन स्मूथ रहेगी)
  if (!isDisplayAsleep) {
    bool hudActive = (lastBrightnessMs > 0 && (now - lastBrightnessMs < HUD_TIMEOUT));
    if (hudActive != wasHudActive) {
      needsRedraw = true;
      wasHudActive = hudActive;
    }

    if (!showIpScreen) {
      if (currentScreen == SCREEN_TIME) {
        struct tm tCheck;
        if (getLocalTime(&tCheck, 0) && tCheck.tm_sec != lastDrawnSec) {
          lastDrawnSec = tCheck.tm_sec;
          needsRedraw = true;
        }
      } else if (weather.valid) {
        // Current Weather और 7-Day Forecast पर वेदर आइकन एनिमेट करने के लिए रिफ्रेश करें
        needsRedraw = true;
      }
    }

    if (needsRedraw) {
      if (showIpScreen) {
        drawIpScreen();
      } else {
        switch (currentScreen) {
          case SCREEN_TIME:
            drawTimeScreen();
            break;
          case SCREEN_CURRENT:
            drawCurrentWeatherScreen();
            break;
          case SCREEN_FORECAST:
            drawForecastScreen();
            break;
        }
      }

      // 8. Render Brightness HUD Overlay if recently adjusted and push frame
      if (hudActive) {
        drawBrightnessOverlay();
      }
      pushFrame();
      needsRedraw = false;
    }
  }

  delay(40); // Responsive ~25 FPS loop for smooth encoder, web server & button tracking
}

// =========================================================================
// SELF-CONTAINED QR CODE GENERATOR (VERSION 2, 25x25, ECC-L, MASK 0)
// =========================================================================
static uint8_t gfMul(uint8_t a, uint8_t b) {
  uint8_t p = 0;
  for (int i = 0; i < 8; i++) {
    if (b & 1) p ^= a;
    bool hi = a & 0x80;
    a <<= 1;
    if (hi) a ^= 0x1D;
    b >>= 1;
  }
  return p;
}

void drawQRCode(int startX, int startY, const char* text, int scale) {
  int len = strlen(text);
  if (len > 32) len = 32; // Version 2-L max byte capacity is 32 bytes

  // 1. Build 34 Data Codewords (Byte Mode = 0100)
  uint8_t codewords[44];
  memset(codewords, 0, sizeof(codewords));
  int bitIdx = 0;
  auto putBits = [&](uint16_t val, int bits) {
    for (int i = bits - 1; i >= 0; i--) {
      if ((val >> i) & 1) codewords[bitIdx >> 3] |= (1 << (7 - (bitIdx & 7)));
      bitIdx++;
    }
  };

  putBits(0x4, 4);
  putBits(len, 8);
  for (int i = 0; i < len; i++) putBits((uint8_t)text[i], 8);
  int remBits = (34 * 8) - bitIdx;
  putBits(0, remBits < 4 ? remBits : 4);
  while (bitIdx & 7) bitIdx++;
  uint8_t padByte = 0xEC;
  while ((bitIdx >> 3) < 34) {
    codewords[bitIdx >> 3] = padByte;
    padByte ^= (0xEC ^ 0x11);
    bitIdx += 8;
  }

  // 2. Reed-Solomon 10 ECC Codewords for Version 2-L
  const uint8_t gen[10] = {216, 194, 159, 111, 199, 94, 95, 113, 157, 193};
  uint8_t ecc[10] = {0};
  for (int i = 0; i < 34; i++) {
    uint8_t factor = codewords[i] ^ ecc[0];
    for (int j = 0; j < 9; j++) ecc[j] = ecc[j + 1] ^ gfMul(gen[j], factor);
    ecc[9] = gfMul(gen[9], factor);
  }
  for (int i = 0; i < 10; i++) codewords[34 + i] = ecc[i];

  // 3. Initialize 25x25 Matrix & Function Patterns
  uint8_t mat[25][25];
  bool boolres[25][25];
  memset(mat, 0, sizeof(mat));
  memset(boolres, 0, sizeof(boolres));

  auto setFinder = [&](int ox, int oy) {
    for (int dy = -1; dy <= 7; dy++) {
      for (int dx = -1; dx <= 7; dx++) {
        int x = ox + dx, y = oy + dy;
        if (x >= 0 && x < 25 && y >= 0 && y < 25) {
          bool dark = (dx >= 0 && dx <= 6 && dy >= 0 && dy <= 6) &&
                      (dx == 0 || dx == 6 || dy == 0 || dy == 6 || (dx >= 2 && dx <= 4 && dy >= 2 && dy <= 4));
          mat[y][x] = dark ? 1 : 0;
          boolres[y][x] = true;
        }
      }
    }
  };
  setFinder(0, 0);
  setFinder(18, 0);
  setFinder(0, 18);

  // Alignment Pattern at (18, 18)
  for (int dy = -2; dy <= 2; dy++) {
    for (int dx = -2; dx <= 2; dx++) {
      bool dark = (abs(dx) == 2 || abs(dy) == 2 || (dx == 0 && dy == 0));
      mat[18 + dy][18 + dx] = dark ? 1 : 0;
      boolres[18 + dy][18 + dx] = true;
    }
  }

  // Timing Patterns
  for (int i = 8; i < 17; i++) {
    mat[6][i] = mat[i][6] = (i % 2 == 0) ? 1 : 0;
    boolres[6][i] = boolres[i][6] = true;
  }

  // Dark Module & Format Reservation
  mat[17][8] = 1;
  boolres[17][8] = true;
  for (int i = 0; i < 9; i++) {
    if (i < 8) {
      boolres[8][24 - i] = true;
      boolres[24 - i][8] = true;
    }
    boolres[8][i] = true;
    boolres[i][8] = true;
  }

  // 4. Place Data Bits + Apply Mask 0 ((row + col) % 2 == 0)
  int bIdx = 0;
  int totalBits = 44 * 8;
  bool up = true;
  for (int right = 24; right >= 1; right -= 2) {
    if (right == 6) right = 5;
    for (int vert = 0; vert < 25; vert++) {
      int y = up ? (24 - vert) : vert;
      for (int j = 0; j < 2; j++) {
        int x = right - j;
        if (!boolres[y][x]) {
          bool bit = false;
          if (bIdx < totalBits) {
            bit = (codewords[bIdx >> 3] >> (7 - (bIdx & 7))) & 1;
            bIdx++;
          }
          if (((y + x) & 1) == 0) bit = !bit;
          mat[y][x] = bit ? 1 : 0;
        }
      }
    }
    up = !up;
  }

  // 5. Place Format Info Bits for ECC Level L + Mask 0 (0x77C4)
  const uint16_t fmt = 0x77C4;
  const int fx1[15] = {0,1,2,3,4,5,7,8,8,8,8,8,8,8,8};
  const int fy1[15] = {8,8,8,8,8,8,8,8,7,5,4,3,2,1,0};
  const int fx2[15] = {8,8,8,8,8,8,8,17,18,19,20,21,22,23,24};
  const int fy2[15] = {24,23,22,21,20,19,18,8,8,8,8,8,8,8,8};
  for (int i = 0; i < 15; i++) {
    uint8_t b = (fmt >> (14 - i)) & 1;
    mat[fy1[i]][fx1[i]] = b;
    mat[fy2[i]][fx2[i]] = b;
  }

  // 6. Render QR Code with White Quiet Zone
  int quiet = 4;
  int boxSize = 25 * scale + quiet * 2;
  spr->fillRoundRect(startX, startY, boxSize, boxSize, 4, TEXT_WHITE);
  for (int y = 0; y < 25; y++) {
    for (int x = 0; x < 25; x++) {
      if (mat[y][x]) {
        spr->fillRect(startX + quiet + x * scale, startY + quiet + y * scale, scale, scale, 0x0000);
      }
    }
  }
}

// =========================================================================
// WEB UI IP ADDRESS + QR CODE SCREEN (TRIGGERED BY CENTER BUTTON LONG PRESS)
// =========================================================================
void drawIpScreen() {
  spr->fillScreen(BG_DARK);
  spr->fillRoundRect(8, 8, 304, 154, 14, CARD_NAVY);
  spr->drawRoundRect(8, 8, 304, 154, 14, ACCENT_CYAN);

  // Header
  drawAlignedText("WEB UI SETUP", 18, 16, 2, ACCENT_CYAN, CARD_NAVY, TL_DATUM, 170);
  spr->drawFastHLine(18, 36, 170, 0x2968);

  String ipStr = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  String fullUrl = "http://" + ipStr;

  if (WiFi.status() == WL_CONNECTED) {
    drawAlignedText("Wi-Fi: " + wifiSSID, 18, 46, 1, TEXT_MUTED, CARD_NAVY, TL_DATUM, 170);
    drawAlignedText("Scan QR or open URL:", 18, 68, 1, ACCENT_MINT, CARD_NAVY, TL_DATUM, 170);
    drawAlignedText(fullUrl, 18, 90, 1, ACCENT_GOLD, CARD_NAVY, TL_DATUM, 170);
  } else {
    drawAlignedText("1. Join Wi-Fi Hotspot:", 18, 46, 1, ACCENT_CORAL, CARD_NAVY, TL_DATUM, 170);
    drawAlignedText("T-Embed-Setup", 18, 64, 2, ACCENT_MINT, CARD_NAVY, TL_DATUM, 170);
    drawAlignedText("2. " + fullUrl, 18, 92, 1, ACCENT_GOLD, CARD_NAVY, TL_DATUM, 170);
  }

  drawAlignedText("Press button to exit", 18, 140, 1, TEXT_MUTED, CARD_NAVY, TL_DATUM, 170);

  // Draw Scannable QR Code on the right side (25x25 * 4px + 8px border = 108x108px)
  drawQRCode(194, 31, fullUrl.c_str(), 4);
}

// =========================================================================
// MOBILE-FRIENDLY WEB SERVER & CITY GEOCODING
// =========================================================================
String urlEncode(const String& str) {
  String encoded = "";
  for (int i = 0; i < str.length(); i++) {
    char c = str.charAt(i);
    if (isalnum(c) || c == '-' || c == '_' || c == '.') {
      encoded += c;
    } else if (c == ' ') {
      encoded += "%20";
    } else {
      char buf[4];
      snprintf(buf, sizeof(buf), "%%%02X", (uint8_t)c);
      encoded += buf;
    }
  }
  return encoded;
}

bool geocodeCity(const String& queryCity, float &outLat, float &outLon, String &outResolvedName) {
  if (WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  String url = "http://geocoding-api.open-meteo.com/v1/search?name=" + urlEncode(queryCity) + "&count=1&language=en&format=json";
  http.begin(url);
  int code = http.GET();

  if (code == 200) {
    String payload = http.getString();
    JsonDocument doc;
    if (!deserializeJson(doc, payload) && doc["results"].is<JsonArray>() && doc["results"].size() > 0) {
      outLat = doc["results"][0]["latitude"];
      outLon = doc["results"][0]["longitude"];
      const char* name = doc["results"][0]["name"];
      if (name) {
        outResolvedName = String(name);
        outResolvedName.toUpperCase();
      }
      http.end();
      return true;
    }
  }
  http.end();
  return false;
}

void handleRoot() {
  lastActivityMs = millis(); // Keep screen awake while user interacts with Web UI

  String html = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
<title>T-Embed Weather Setup</title>
<style>
:root { --bg: #0b1120; --card: #152238; --accent: #00f2fe; --gold: #ffb703; --text: #f8fafc; --muted: #94a3b8; }
- { box-sizing: border-box; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; }
body { background: var(--bg); color: var(--text); margin: 0; padding: 16px; display: flex; justify-content: center; }
.container { width: 100%; max-width: 420px; background: var(--card); padding: 22px; border-radius: 16px; border: 1px solid rgba(0,242,254,0.25); box-shadow: 0 10px 25px rgba(0,0,0,0.5); }
h2 { margin: 0 0 4px 0; color: var(--accent); font-size: 1.35rem; text-align: center; }
.sub { text-align: center; color: var(--muted); font-size: 0.85rem; margin-bottom: 18px; }
.section { margin-top: 16px; padding-top: 12px; border-top: 1px solid rgba(255,255,255,0.08); }
.sec-title { font-size: 0.8rem; text-transform: uppercase; letter-spacing: 1px; color: var(--gold); margin-bottom: 10px; font-weight: 700; }
label { display: block; font-size: 0.85rem; color: var(--muted); margin: 10px 0 4px; }
input, select { width: 100%; padding: 11px 12px; border-radius: 10px; border: 1px solid #2e4057; background: #0b1325; color: var(--text); font-size: 0.95rem; outline: none; }
input:focus, select:focus { border-color: var(--accent); }
.row { display: flex; gap: 10px; }
.row > div { flex: 1; }
.hint { font-size: 0.76rem; color: var(--muted); margin-top: 4px; }
.toggle-row { display: flex; justify-content: space-between; align-items: center; padding: 10px 0; }
.toggle-label { font-size: 0.9rem; color: var(--text); }
.toggle-sub { font-size: 0.75rem; color: var(--muted); display: block; margin-top: 2px; }
.switch { position: relative; display: inline-block; width: 48px; height: 26px; flex-shrink: 0; margin: 0; }
.switch input { opacity: 0; width: 0; height: 0; }
.slider { position: absolute; cursor: pointer; top: 0; left: 0; right: 0; bottom: 0; background-color: #2e4057; transition: .25s; border-radius: 26px; }
.slider:before { position: absolute; content: ""; height: 20px; width: 20px; left: 3px; bottom: 3px; background-color: var(--text); transition: .25s; border-radius: 50%; }
input:checked + .slider { background: linear-gradient(135deg, #00f2fe, #4facfe); }
input:checked + .slider:before { transform: translateX(22px); background-color: #06101e; }
button { width: 100%; margin-top: 22px; padding: 13px; border: none; border-radius: 10px; background: linear-gradient(135deg, #00f2fe, #4facfe); color: #06101e; font-size: 1rem; font-weight: 700; cursor: pointer; }
button:active { opacity: 0.85; }
</style>
</head>
<body>
<div class="container">
<h2>T-Embed Weather Station</h2>
<div class="sub">Mobile Configuration Portal</div>
<form action="/save" method="POST">
<div class="section" style="border-top:none; margin-top:0; padding-top:0;">
<div class="sec-title">Wi-Fi Credentials</div>
<label>Wi-Fi SSID</label>
<input type="text" name="ssid" value=")rawliteral" + wifiSSID + R"rawliteral(" required>
<label>Wi-Fi Password</label>
<input type="password" name="pass" value=")rawliteral" + wifiPass + R"rawliteral(" placeholder="Enter Wi-Fi password">
</div>

<div class="section">
<div class="sec-title">Country & Time Zone</div>
<label>Select Time Zone</label>
<select name="gmt">
<option value="19800" )rawliteral" + String(gmtOffsetSec == 19800 ? "selected" : "") + R"rawliteral(>India (IST / UTC+5:30)</option>
<option value="0" )rawliteral" + String(gmtOffsetSec == 0 ? "selected" : "") + R"rawliteral(>UK / UTC (UTC+0:00)</option>
<option value="3600" )rawliteral" + String(gmtOffsetSec == 3600 ? "selected" : "") + R"rawliteral(>Central Europe (CET / UTC+1:00)</option>
<option value="7200" )rawliteral" + String(gmtOffsetSec == 7200 ? "selected" : "") + R"rawliteral(>Eastern Europe (EET / UTC+2:00)</option>
<option value="10800" )rawliteral" + String(gmtOffsetSec == 10800 ? "selected" : "") + R"rawliteral(>Middle East / Moscow (UTC+3:00)</option>
<option value="14400" )rawliteral" + String(gmtOffsetSec == 14400 ? "selected" : "") + R"rawliteral(>UAE / Dubai (GST / UTC+4:00)</option>
<option value="20700" )rawliteral" + String(gmtOffsetSec == 20700 ? "selected" : "") + R"rawliteral(>Nepal (NPT / UTC+5:45)</option>
<option value="21600" )rawliteral" + String(gmtOffsetSec == 21600 ? "selected" : "") + R"rawliteral(>Bangladesh (BST / UTC+6:00)</option>
<option value="25200" )rawliteral" + String(gmtOffsetSec == 25200 ? "selected" : "") + R"rawliteral(>Thailand / Vietnam (ICT / UTC+7:00)</option>
<option value="28800" )rawliteral" + String(gmtOffsetSec == 28800 ? "selected" : "") + R"rawliteral(>Singapore / China / AWST (UTC+8:00)</option>
<option value="32400" )rawliteral" + String(gmtOffsetSec == 32400 ? "selected" : "") + R"rawliteral(>Japan / Korea (JST / UTC+9:00)</option>
<option value="36000" )rawliteral" + String(gmtOffsetSec == 36000 ? "selected" : "") + R"rawliteral(>Australia Sydney (AEST / UTC+10:00)</option>
<option value="-18000" )rawliteral" + String(gmtOffsetSec == -18000 ? "selected" : "") + R"rawliteral(>US Eastern (EST / UTC-5:00)</option>
<option value="-21600" )rawliteral" + String(gmtOffsetSec == -21600 ? "selected" : "") + R"rawliteral(>US Central (CST / UTC-6:00)</option>
<option value="-25200" )rawliteral" + String(gmtOffsetSec == -25200 ? "selected" : "") + R"rawliteral(>US Mountain (MST / UTC-7:00)</option>
<option value="-28800" )rawliteral" + String(gmtOffsetSec == -28800 ? "selected" : "") + R"rawliteral(>US Pacific (PST / UTC-8:00)</option>
</select>
</div>

<div class="section">
<div class="sec-title">Weather City & Location</div>
<label>City Name (Auto-Locates Coordinates)</label>
<input type="text" name="city" value=")rawliteral" + cityName + R"rawliteral(" required>
<div class="hint">Type any city name (e.g. Mumbai, Bengaluru, London) to auto-fetch coordinates, or edit Lat/Lon below:</div>
<div class="row">
<div>
<label>Latitude</label>
<input type="number" step="0.0001" name="lat" value=")rawliteral" + String(latitude, 4) + R"rawliteral(">
</div>
<div>
<label>Longitude</label>
<input type="number" step="0.0001" name="lon" value=")rawliteral" + String(longitude, 4) + R"rawliteral(">
</div>
</div>
</div>

<div class="section">
<div class="sec-title">Display & Screen Controls</div>
<div class="toggle-row">
<div>
<span class="toggle-label">Auto Sleep</span>
<span class="toggle-sub">Turn off display after 30s inactivity</span>
</div>
<label class="switch">
<input type="checkbox" name="autosleep" value="1" )rawliteral" + String(autoSleepEnabled ? "checked" : "") + R"rawliteral(>
<span class="slider"></span>
</label>
</div>
<div class="toggle-row">
<div>
<span class="toggle-label">Auto Cycle Screens</span>
<span class="toggle-sub">Switch between 3 screens every 5s</span>
</div>
<label class="switch">
<input type="checkbox" name="autocycle" value="1" )rawliteral" + String(autoCycleEnabled ? "checked" : "") + R"rawliteral(>
<span class="slider"></span>
</label>
</div>
</div>

<button type="submit">Save & Apply Settings</button>
</form>
</div>
</body>
</html>
)rawliteral";

  server.send(200, "text/html", html);
}

void handleSave() {
  lastActivityMs = millis();

  String newSsid = server.arg("ssid");
  String newPass = server.arg("pass");
  String newCity = server.arg("city");
  long newGmt = server.arg("gmt").toInt();
  float newLat = server.arg("lat").toFloat();
  float newLon = server.arg("lon").toFloat();
  autoSleepEnabled = server.hasArg("autosleep");
  autoCycleEnabled = server.hasArg("autocycle");

  newCity.trim();
  String upperCity = newCity;
  upperCity.toUpperCase();

  bool wifiChanged = (newSsid.length() > 0 && (newSsid != wifiSSID || newPass != wifiPass));

  // If the city name changed or coordinates are zero, auto-geocode the city name via Open-Meteo
  if (newCity.length() > 0 && (upperCity != cityName || (fabs(newLat - latitude) < 0.0001 && fabs(newLon - longitude) < 0.0001))) {
    float geoLat, geoLon;
    String resolvedCity;
    if (geocodeCity(newCity, geoLat, geoLon, resolvedCity)) {
      newLat = geoLat;
      newLon = geoLon;
      upperCity = resolvedCity;
    }
  }

  if (newSsid.length() > 0) wifiSSID = newSsid;
  if (newPass.length() > 0) wifiPass = newPass;
  if (upperCity.length() > 0) cityName = upperCity;
  latitude = newLat;
  longitude = newLon;
  gmtOffsetSec = newGmt;

  // Save to ESP32 NVS Flash
  prefs.begin("weather_cfg", false);
  prefs.putString("ssid", wifiSSID);
  prefs.putString("pass", wifiPass);
  prefs.putString("city", cityName);
  prefs.putFloat("lat", latitude);
  prefs.putFloat("lon", longitude);
  prefs.putLong("gmt", gmtOffsetSec);
  prefs.putBool("autosleep", autoSleepEnabled);
  prefs.putBool("autocycle", autoCycleEnabled);
  prefs.end();

  lastCycleMs = millis();
  if (!autoSleepEnabled && isDisplayAsleep) {
    isDisplayAsleep = false;
    setBacklight(brightness);
  }

  // Apply Time Zone & Refresh Weather immediately
  configTime(gmtOffsetSec, 0, "pool.ntp.org", "time.google.com", "in.pool.ntp.org");
  struct tm timeinfo;
  getLocalTime(&timeinfo, 1500);
  fetchWeather();
  needsRedraw = true;

  String resp = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<meta http-equiv="refresh" content="3;url=/">
<style>body{background:#0b1120;color:#f8fafc;font-family:sans-serif;display:flex;justify-content:center;align-items:center;height:90vh;margin:0;}
.box{background:#152238;padding:24px;border-radius:16px;text-align:center;border:1px solid #00f2fe;max-width:340px;}
h3{color:#27f4a0;margin:0 0 10px;}p{color:#94a3b8;font-size:0.9rem;}</style></head>
<body><div class="box"><h3>Settings Saved!</h3><p>City: )rawliteral" + cityName + " (" + String(latitude, 2) + ", " + String(longitude, 2) + R"rawliteral()</p><p>Redirecting back...</p></div></body></html>
)rawliteral";

  server.send(200, "text/html", resp);

  if (wifiChanged) {
    delay(800);
    ESP.restart();
  }
}

void setupWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.begin();
}

// =========================================================================
// POWER OFF & DEEP SLEEP HANDLER
// =========================================================================
void powerOffDevice() {
  setBacklight(brightness);
  // Show Power-Off Confirmation Card
  spr->fillScreen(BG_DARK);
  spr->fillRoundRect(40, 45, 240, 80, 12, CARD_NAVY);
  spr->drawRoundRect(40, 45, 240, 80, 12, ACCENT_CORAL);
  drawAlignedText("POWERING OFF...", 160, 72, 2, ACCENT_CORAL, CARD_NAVY, MC_DATUM);
  drawAlignedText("Release button to sleep", 160, 102, 1, TEXT_MUTED, CARD_NAVY, MC_DATUM);
  pushFrame();

  // Wait for user to release buttons so it doesn't immediately wake up
  while (digitalRead(PIN_BACK_BTN) == LOW || digitalRead(PIN_ENCODER_BTN) == LOW) {
    delay(20);
  }
  delay(300);

  // Fade out backlight smoothly
  for (int b = brightness; b >= 0; b -= 15) {
    setBacklight(b);
    delay(15);
  }
  setBacklight(0);

  // Put TFT controller into sleep
  tft.enableSleep(true);
  delay(50);

  // Cut battery power rail (turns off completely when running on battery)
  digitalWrite(BOARD_PWR_EN, LOW);

  // Fallback if plugged into USB: Enter ESP32-S3 Deep Sleep, wake on Center Button (GPIO 0)
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);
  esp_deep_sleep_start();
}

// =========================================================================
// BRIGHTNESS HUD OVERLAY
// =========================================================================
void drawBrightnessOverlay() {
  int pct = map(brightness, 15, 255, 5, 100);
  int barW = map(brightness, 15, 255, 6, 112);

  spr->fillRoundRect(60, 14, 200, 34, 8, BG_DARK);
  spr->drawRoundRect(60, 14, 200, 34, 8, ACCENT_GOLD);

  drawAlignedText("BRI " + String(pct) + "%", 72, 27, 1, ACCENT_GOLD, BG_DARK, TL_DATUM);

  // Progress Bar Track & Fill
  spr->fillRoundRect(136, 25, 112, 12, 4, CARD_NAVY);
  spr->fillRoundRect(136, 25, barW, 12, 4, ACCENT_GOLD);
}

// =========================================================================
// SCREEN 1: FULL-SCREEN INDIA TIME (12H FORMAT) + DAY & DATE
// =========================================================================
void drawTimeScreen() {
  spr->fillScreen(BG_DARK);

  // Decorative background card
  spr->fillRoundRect(8, 8, 304, 154, 14, CARD_NAVY);
  spr->drawRoundRect(8, 8, 304, 154, 14, ACCENT_CYAN);

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 0)) {
    drawAlignedText("Waiting for NTP Sync...", 160, 85, 2, ACCENT_CORAL, CARD_NAVY, MC_DATUM, 280);
    return;
  }

  // Top Header: Location & Battery Percentage
  drawAlignedText(cityName, 20, 16, 2, ACCENT_CYAN, CARD_NAVY, TL_DATUM, 170);

  char batStr[12];
  snprintf(batStr, sizeof(batStr), "BAT %d%%", getBatteryPercentage());
  drawAlignedText(batStr, 300, 16, 2, ACCENT_MINT, CARD_NAVY, TR_DATUM);

  // Format 12-hour time
  int hour12 = timeinfo.tm_hour % 12;
  if (hour12 == 0) hour12 = 12;
  const char* ampm = (timeinfo.tm_hour >= 12) ? "PM" : "AM";

  char hmStr[10];
  snprintf(hmStr, sizeof(hmStr), "%02d:%02d", hour12, timeinfo.tm_min);

  char secStr[6];
  snprintf(secStr, sizeof(secStr), ":%02d", timeinfo.tm_sec);

  // Large Hours:Minutes
  drawAlignedText(hmStr, 220, 82, 6, TEXT_WHITE, CARD_NAVY, MR_DATUM);

  // Seconds & AM/PM badge on the right
  drawAlignedText(secStr, 228, 56, 3, ACCENT_GOLD, CARD_NAVY, TL_DATUM);

  spr->fillRoundRect(228, 84, 54, 26, 6, ACCENT_CORAL);
  drawAlignedText(ampm, 255, 97, 2, TEXT_WHITE, ACCENT_CORAL, MC_DATUM);

  // Bottom Divider & Day + Date
  spr->drawFastHLine(20, 118, 280, 0x2968);

  char dateStr[40];
  strftime(dateStr, sizeof(dateStr), "%a %d %b %Y", &timeinfo);
  drawAlignedText(dateStr, 160, 136, 2, ACCENT_GOLD, CARD_NAVY, MC_DATUM, 292);

  drawProgressDots(0);
}

// =========================================================================
// SCREEN 2: CURRENT LIVE WEATHER
// =========================================================================
void drawCurrentWeatherScreen() {
  spr->fillScreen(BG_DARK);
  spr->fillRoundRect(8, 8, 304, 154, 14, CARD_PURPLE);
  spr->drawRoundRect(8, 8, 304, 154, 14, ACCENT_GOLD);

  // Header
  drawAlignedText(cityName + " | NOW", 20, 16, 2, ACCENT_GOLD, CARD_PURPLE, TL_DATUM, 280);

  if (!weather.valid) {
    drawAlignedText("Fetching Weather Data...", 160, 85, 2, TEXT_WHITE, CARD_PURPLE, MC_DATUM, 280);
    drawProgressDots(1);
    return;
  }

  // Left: Custom Weather Graphic
  drawWeatherIcon(72, 78, weather.weatherCode, 2);

  // Center-Right: Big Temperature
  char tempStr[12];
  snprintf(tempStr, sizeof(tempStr), "%.1f", weather.currentTemp);

  drawAlignedText(tempStr, 132, 46, 4, TEXT_WHITE, CARD_PURPLE, TL_DATUM);
  int16_t x1, y1;
  uint16_t tw, th;
  spr->setTextSize(4);
  spr->getTextBounds(tempStr, 0, 0, &x1, &y1, &tw, &th);
  drawAlignedText("o", 134 + tw, 42, 2, ACCENT_GOLD, CARD_PURPLE, TL_DATUM);
  drawAlignedText("C", 146 + tw, 48, 3, ACCENT_GOLD, CARD_PURPLE, TL_DATUM);

  // Condition Label
  drawAlignedText(getWeatherDesc(weather.weatherCode), 132, 92, 2, ACCENT_CYAN, CARD_PURPLE, TL_DATUM, 170);

  // Bottom 3 Metric Pills (Feels Like | Humidity | Wind)
  spr->fillRoundRect(18, 122, 90, 28, 6, BG_DARK);
  spr->fillRoundRect(115, 122, 90, 28, 6, BG_DARK);
  spr->fillRoundRect(212, 122, 90, 28, 6, BG_DARK);

  char buf[24];
  snprintf(buf, sizeof(buf), "Feels %.0fC", weather.feelsLike);
  drawAlignedText(buf, 63, 136, 1, ACCENT_CORAL, BG_DARK, MC_DATUM);

  snprintf(buf, sizeof(buf), "Hum %d%%", weather.humidity);
  drawAlignedText(buf, 160, 136, 1, ACCENT_CYAN, BG_DARK, MC_DATUM);

  snprintf(buf, sizeof(buf), "%.0f km/h", weather.windSpeed);
  drawAlignedText(buf, 257, 136, 1, ACCENT_MINT, BG_DARK, MC_DATUM);

  drawProgressDots(1);
}

// =========================================================================
// SCREEN 3: 7-DAY FORECAST
// =========================================================================
void drawForecastScreen() {
  spr->fillScreen(BG_DARK);
  spr->fillRoundRect(6, 6, 308, 158, 12, CARD_TEAL);
  spr->drawRoundRect(6, 6, 308, 158, 12, ACCENT_MINT);

  drawAlignedText("7-DAY FORECAST (" + cityName + ")", 16, 14, 2, ACCENT_MINT, CARD_TEAL, TL_DATUM, 288);

  if (!weather.valid) {
    drawAlignedText("No Forecast Loaded", 160, 85, 2, TEXT_WHITE, CARD_TEAL, MC_DATUM, 280);
    drawProgressDots(2);
    return;
  }

  // 7 Columns across 300px width (42px per column)
  int startX = 13;
  int colW = 42;

  for (int i = 0; i < 7; i++) {
    int x = startX + i * colW;
    uint16_t colBg = (i == 0) ? 0x120A : BG_DARK;

    spr->fillRoundRect(x, 34, colW - 3, 118, 6, colBg);
    if (i == 0) {
      spr->drawRoundRect(x, 34, colW - 3, 118, 6, ACCENT_GOLD);
    }

    int cx = x + (colW - 3) / 2;

    // Day Name
    drawAlignedText(i == 0 ? "TDY" : weather.days[i].dayName, cx, 46, 1, i == 0 ? ACCENT_GOLD : ACCENT_CYAN, colBg, MC_DATUM);

    // Mini Weather Icon
    drawWeatherIcon(cx, 78, weather.days[i].code, 1);

    // Max Temp
    char maxBuf[8];
    snprintf(maxBuf, sizeof(maxBuf), "%.0f%c", weather.days[i].maxTemp, (char)248);
    drawAlignedText(maxBuf, cx, 112, 1, ACCENT_CORAL, colBg, MC_DATUM);

    // Min Temp
    char minBuf[8];
    snprintf(minBuf, sizeof(minBuf), "%.0f%c", weather.days[i].minTemp, (char)248);
    drawAlignedText(minBuf, cx, 134, 1, TEXT_MUTED, colBg, MC_DATUM);
  }

  drawProgressDots(2);
}

// =========================================================================
// OPEN-METEO API FETCH (NO API KEY NEEDED)
// =========================================================================
void fetchWeather() {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  String url = "http://api.open-meteo.com/v1/forecast?latitude=" + String(latitude, 4) +
               "&longitude=" + String(longitude, 4) +
               "&current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,wind_speed_10m" +
               "&daily=weather_code,temperature_2m_max,temperature_2m_min" +
               "&timezone=auto";

  http.begin(url);
  int httpCode = http.GET();

  if (httpCode == 200) {
    String payload = http.getString();
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload);

    if (!error) {
      weather.currentTemp = doc["current"]["temperature_2m"];
      weather.feelsLike = doc["current"]["apparent_temperature"];
      weather.humidity = doc["current"]["relative_humidity_2m"];
      weather.windSpeed = doc["current"]["wind_speed_10m"];
      weather.weatherCode = doc["current"]["weather_code"];

      JsonArray dates = doc["daily"]["time"];
      JsonArray codes = doc["daily"]["weather_code"];
      JsonArray maxT = doc["daily"]["temperature_2m_max"];
      JsonArray minT = doc["daily"]["temperature_2m_min"];

      for (int i = 0; i < 7 && i < (int)dates.size(); i++) {
        weather.days[i].dayName = getShortDayName(dates[i]);
        weather.days[i].code = codes[i];
        weather.days[i].maxTemp = maxT[i];
        weather.days[i].minTemp = minT[i];
      }
      weather.valid = true;
    }
  }
  http.end();
}

// =========================================================================
// ANIMATED VECTOR WEATHER ICONS & HELPERS
// =========================================================================
void drawWeatherIcon(int cx, int cy, int code, int s) {
  unsigned long nowMs = millis() + (unsigned long)(cx * 37);
  float t = nowMs / 1000.0f;

  if (code == 0 || code == 1) {
    // Clear / Sunny: घूमती किरणें + हल्की पल्स
    float angleOffset = fmod(t * 28.0f, 360.0f);
    float pulse = sin(t * 3.0f) * 1.2f * s;

    spr->fillCircle(cx, cy, (int)(9 * s + pulse * 0.4f), ACCENT_GOLD);
    for (int a = 0; a < 360; a += 45) {
      float rad = (a + angleOffset) * 0.0174533f;
      int r1 = 12 * s;
      int r2 = (int)(15.5f * s + ((a % 90 == 0) ? pulse : -pulse));
      int x1 = cx + (int)(cos(rad) * r1);
      int y1 = cy + (int)(sin(rad) * r1);
      int x2 = cx + (int)(cos(rad) * r2);
      int y2 = cy + (int)(sin(rad) * r2);
      spr->drawLine(x1, y1, x2, y2, ACCENT_GOLD);
      if (s >= 2) {
        spr->drawLine(x1 + 1, y1, x2 + 1, y2, ACCENT_GOLD);
      }
    }

  } else if (code == 2) {
    // Partly Cloudy: पीछे घूमता सूरज + आगे तैरता बादल
    int sunX = cx - 5 * s;
    int sunY = cy - 5 * s;
    float angleOffset = fmod(t * 25.0f, 360.0f);
    spr->fillCircle(sunX, sunY, 6 * s, ACCENT_GOLD);
    for (int a = 0; a < 360; a += 60) {
      float rad = (a + angleOffset) * 0.0174533f;
      spr->drawLine(sunX + (int)(cos(rad) * 8 * s), sunY + (int)(sin(rad) * 8 * s),
                    sunX + (int)(cos(rad) * 10 * s), sunY + (int)(sin(rad) * 10 * s), ACCENT_GOLD);
    }

    int drift = (int)round(sin(t * 1.8f) * 2.0f * s);
    int ccx = cx + drift;
    spr->fillCircle(ccx - 4 * s, cy + 2 * s, 6 * s, TEXT_WHITE);
    spr->fillCircle(ccx + 5 * s, cy + 2 * s, 5 * s, TEXT_WHITE);
    spr->fillRoundRect(ccx - 10 * s, cy + 1 * s, 20 * s, 7 * s, 3 * s, TEXT_WHITE);

  } else if (code == 3) {
    // Overcast: दो लेयर के तैरते बादल
    int backDrift = (int)round(sin(t * 1.2f + 1.5f) * 2.0f * s);
    int frontDrift = (int)round(sin(t * 1.8f) * 2.0f * s);

    int bx = cx + backDrift + 2 * s;
    spr->fillCircle(bx - 3 * s, cy - 3 * s, 6 * s, TEXT_MUTED);
    spr->fillCircle(bx + 4 * s, cy - 3 * s, 5 * s, TEXT_MUTED);
    spr->fillRoundRect(bx - 9 * s, cy - 3 * s, 18 * s, 6 * s, 2 * s, TEXT_MUTED);

    int fx = cx + frontDrift - 1 * s;
    spr->fillCircle(fx - 4 * s, cy + 3 * s, 6 * s, TEXT_WHITE);
    spr->fillCircle(fx + 4 * s, cy + 3 * s, 5 * s, TEXT_WHITE);
    spr->fillRoundRect(fx - 10 * s, cy + 2 * s, 20 * s, 7 * s, 3 * s, TEXT_WHITE);

  } else if (code == 45 || code == 48) {
    // Fog / Haze: बादल + सरकती धुंध की लकीरें
    spr->fillCircle(cx - 4 * s, cy - 4 * s, 6 * s, TEXT_MUTED);
    spr->fillCircle(cx + 4 * s, cy - 4 * s, 5 * s, TEXT_MUTED);
    spr->fillRoundRect(cx - 10 * s, cy - 5 * s, 20 * s, 7 * s, 3 * s, TEXT_MUTED);

    for (int i = 0; i < 3; i++) {
      int slide = (int)round(sin(t * 1.8f + i * 1.4f) * 3.0f * s);
      int yPos = cy + (5 + i * 4) * s;
      int w = (16 - i * 2) * s;
      spr->fillRoundRect(cx - w / 2 + slide, yPos, w, max(1, s), s, (i == 1) ? TEXT_WHITE : TEXT_MUTED);
    }

  } else if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) {
    // Rain / Drizzle / Showers: हल्का हिलता बादल + गिरती बूँदें
    int bob = (int)round(sin(t * 2.4f) * 1.0f * s);
    int cloudY = cy - 4 * s + bob;

    spr->fillCircle(cx - 4 * s, cloudY, 6 * s, TEXT_MUTED);
    spr->fillCircle(cx + 5 * s, cloudY, 5 * s, TEXT_MUTED);
    spr->fillRoundRect(cx - 10 * s, cloudY - 1 * s, 20 * s, 7 * s, 3 * s, TEXT_MUTED);

    int xOffsets[3] = {-6, 0, 6};
    for (int i = 0; i < 3; i++) {
      float p = fmod(t * 1.9f + i * 0.33f, 1.0f);
      int dy = (int)(p * 7.0f * s);
      int dx = dy / 3;
      int x0 = cx + xOffsets[i] * s - dx;
      int y0 = cy + 4 * s + dy;
      int x1 = x0 - 2 * s;
      int y1 = y0 + 4 * s;
      spr->drawLine(x0, y0, x1, y1, ACCENT_CYAN);
      if (s >= 2) {
        spr->drawLine(x0 + 1, y0, x1 + 1, y1, ACCENT_CYAN);
      }
    }

  } else if ((code >= 71 && code <= 77) || code == 85 || code == 86) {
    // Snow: बादल + झूलकर गिरते बर्फ़ के कण
    spr->fillCircle(cx - 4 * s, cy - 4 * s, 6 * s, TEXT_WHITE);
    spr->fillCircle(cx + 5 * s, cy - 4 * s, 5 * s, TEXT_WHITE);
    spr->fillRoundRect(cx - 10 * s, cy - 5 * s, 20 * s, 7 * s, 3 * s, TEXT_WHITE);

    int xOffsets[3] = {-6, 0, 6};
    for (int i = 0; i < 3; i++) {
      float p = fmod(t * 0.9f + i * 0.33f, 1.0f);
      int dy = (int)(p * 9.0f * s);
      int sway = (int)round(sin(t * 3.0f + i * 2.0f) * 2.0f * s);
      spr->fillCircle(cx + xOffsets[i] * s + sway, cy + 5 * s + dy, max(1, s), TEXT_WHITE);
    }

  } else if (code >= 95) {
    // Thunderstorm: डबल-फ्लैश बिजली + बारिश
    unsigned long cycle = nowMs % 2400;
    bool flash = (cycle < 130) || (cycle >= 210 && cycle < 310);
    uint16_t cloudCol = flash ? TEXT_WHITE : TEXT_MUTED;

    spr->fillCircle(cx - 4 * s, cy - 5 * s, 6 * s, cloudCol);
    spr->fillCircle(cx + 5 * s, cy - 5 * s, 5 * s, cloudCol);
    spr->fillRoundRect(cx - 10 * s, cy - 6 * s, 20 * s, 7 * s, 3 * s, cloudCol);

    int rainX[2] = {-7, 7};
    for (int i = 0; i < 2; i++) {
      float p = fmod(t * 2.2f + i * 0.5f, 1.0f);
      int dy = (int)(p * 7.0f * s);
      int x0 = cx + rainX[i] * s - (dy / 3);
      int y0 = cy + 3 * s + dy;
      spr->drawLine(x0, y0, x0 - 2 * s, y0 + 4 * s, ACCENT_CYAN);
    }

    if (flash) {
      spr->fillTriangle(cx + 1 * s, cy + 1 * s, cx - 4 * s, cy + 8 * s, cx + 2 * s, cy + 8 * s, ACCENT_GOLD);
      spr->fillTriangle(cx + 2 * s, cy + 7 * s, cx - 3 * s, cy + 15 * s, cx + 4 * s, cy + 7 * s, ACCENT_GOLD);
    }

  } else {
    // Default Cloud: हल्का पल्स
    int r = (int)(7 * s + sin(t * 2.0f) * s);
    spr->fillCircle(cx, cy, r, ACCENT_CYAN);
  }
}

String getWeatherDesc(int code) {
  if (code == 0) return "Clear Sky";
  if (code == 1 || code == 2) return "Partly Cloudy";
  if (code == 3) return "Overcast";
  if (code == 45 || code == 48) return "Fog / Haze";
  if (code >= 51 && code <= 57) return "Light Drizzle";
  if (code >= 61 && code <= 67) return "Rain";
  if (code >= 80 && code <= 82) return "Rain Showers";
  if (code >= 95) return "Thunderstorm";
  return "Fair Weather";
}

String getShortDayName(const char* dateStr) {
  int y, m, d;
  if (sscanf(dateStr, "%d-%d-%d", &y, &m, &d) != 3) return "---";
  struct tm t = {0};
  t.tm_year = y - 1900;
  t.tm_mon = m - 1;
  t.tm_mday = d;
  mktime(&t);
  const char* days[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
  return String(days[t.tm_wday]);
}

void drawProgressDots(int activeIdx) {
  for (int i = 0; i < 3; i++) {
    int x = 148 + i * 12;
    if (i == activeIdx) {
      spr->fillCircle(x, 157, 3, ACCENT_GOLD);
    } else {
      spr->drawCircle(x, 157, 2, TEXT_MUTED);
    }
  }
}