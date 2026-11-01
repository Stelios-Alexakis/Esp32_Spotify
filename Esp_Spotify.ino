#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <TJpg_Decoder.h>
#include <base64.h>
#include <time.h>

// ================= 1. ΣΤΟΙΧΕΙΑ ΧΡΗΣΤΗ =================
#if __has_include("secrets.h")
  #include "secrets.h"
#else
  #error "Missing secrets.h! Please copy secrets.h.example to secrets.h and enter your Wi-Fi and Spotify credentials."
#endif

const char* WIFI_SSID     = SECRET_WIFI_SSID;
const char* WIFI_PASSWORD = SECRET_WIFI_PASSWORD;

const char* CLIENT_ID     = SECRET_CLIENT_ID;
const char* CLIENT_SECRET = SECRET_CLIENT_SECRET;
const char* REFRESH_TOKEN = SECRET_REFRESH_TOKEN;

// Πόλη για τον καιρό (βάλε την πόλη σου στα αγγλικά, π.χ. "Thessaloniki", "Athens", "Larissa", "Patra", "Chania" κλπ)
// Αν το αφήσεις κενό "", θα προσπαθήσει αυτόματα βάσει IP.
const char* CITY_NAME     = SECRET_CITY_NAME;

// ================= 2. PINS ΟΘΟΝΗΣ & ΠΕΡΙΦΕΡΕΙΑΚΩΝ =====
// Οθόνη TFT 1.8" (βάσει της συνδεσμολογίας σου στο 30-pin ESP32)
#define TFT_CS    5   // Pin 2 οθόνης -> D5
#define TFT_DC    4   // Pin 3 οθόνης -> D4
#define TFT_RST   22  // Pin 1 οθόνης -> D22
// Τα DIN (D23) και CLK (D18) χρησιμοποιούνται αυτόματα από το Hardware VSPI

// Rotary Encoder
#define ENC_CLK   32  // D32
#define ENC_DT    33  // D33
#define ENC_SW    25  // D25

// 4-Bit Button Module
#define BTN_PREV  26  // K1 -> D26
#define BTN_PLAY  27  // K2 -> D27
#define BTN_NEXT  14  // K3 -> D14
#define BTN_SHUF  13  // K4 -> D13 (Απλό πάτημα: Shuffle | Παρατεταμένο: Repeat)

// ================= 3. ΧΡΩΜΑΤΙΚΗ ΠΑΛΕΤΑ (RGB565) =======
#define UI_BLACK      0x0000
#define UI_WHITE      0xFFFF
#define UI_GRAY       0x4208
#define UI_LIGHT_GRAY 0xAD55
#define UI_GREEN      0x1DB9  // Spotify Green
#define UI_CYAN       0x07FF

// ================= 4. ΚΑΘΟΛΙΚΕΣ ΜΕΤΑΒΛΗΤΕΣ ============
Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);

String accessToken = "";
unsigned long lastTokenTime      = 0;
unsigned long lastCheckTime      = 0;
unsigned long lastProgressUpdate = 0;
unsigned long lastScrollUpdate   = 0;

// Ρύθμιση ταχύτητας κύλισης κειμένου (σε ms ανά χαρακτήρα)
const unsigned long SCROLL_INTERVAL = 600; // Αύξησέ το (π.χ. 800) για ακόμα πιο αργή κύλιση

// Κατάσταση & Local Recalibration Χρόνου
bool isPlaying       = false;
bool shuffleState    = false;
String repeatState   = "off"; // "off", "context", "track"
bool hasValidData    = false;
long serverProgress  = 0;
long trackDuration   = 1;
unsigned long lastServerUpdate = 0;

String currentTrackId = "";
String currentTitle   = "";
String currentArtist  = "";
String currentAlbum   = "";

// Μεταβλητές Κυλιόμενου Κειμένου (RDS / Marquee Loop)
int scrollIdxTitle  = 0;
int scrollIdxArtist = 0;
int scrollIdxAlbum  = 0;
const int MAX_CHARS = 13;
const String SCROLL_SEP = "   *   "; // Διαχωριστικό ανάμεσα στις επαναλήψεις

// Ένταση & Rotary Encoder
int currentVolume    = 50;
int savedMuteVolume  = 50;
bool isMuted         = false;
volatile int encoderDelta = 0;
volatile unsigned long lastEncISR = 0;
unsigned long lastVolumeChange = 0;
bool volumePendingSend = false;

// UI Optimization (αποφυγή τρεμοπαίγματος)
int lastProgressWidth    = -1;
String lastDisplayedTime = "";

// Buffer στη RAM για το 64x64 JPEG εξώφυλλο
uint8_t jpgBuffer[10240];

// ================= Screensaver / Αναλογικό Ρολόι & Καιρός =================
bool inScreensaver = false;
unsigned long lastPlaybackActiveTime = 0;
const unsigned long IDLE_TIMEOUT_MS  = 45000; // 45 δευτερόλεπτα αδράνειας για είσοδο σε Screensaver

// Συντεταγμένες και διαστάσεις αναλογικού ρολογιού (160x128)
const int CLOCK_X = 80;
const int CLOCK_Y = 38;
const int CLOCK_R = 28;
const int H_LEN   = 15;
const int M_LEN   = 21;
const int S_LEN   = 23;

int lastSecX  = -1, lastSecY  = -1;
int lastMinX  = -1, lastMinY  = -1;
int lastHourX = -1, lastHourY = -1;

String lastClockDateStr    = "";
String lastClockLocStr     = "";
String lastClockWeatherStr = "";

// Τοποθεσία (βάσει IP) & Καιρός (Open-Meteo)
String userCity           = "Athens";
float userLat             = 37.98;
float userLon             = 23.73;
float currentTemp         = 18.0;
String currentWeatherDesc = "Clear Sky";
bool hasWeatherData       = false;
unsigned long lastWeatherUpdate = 0;

// ================= Menu Mode (Queue / Playlists) =========
enum MenuState { MENU_NONE, MENU_MAIN, MENU_QUEUE, MENU_PLAYLISTS };
MenuState menuState = MENU_NONE;

#define MAX_MENU_ITEMS 25
#define MENU_VISIBLE   5
#define MENU_MAX_CHARS 23
#define MENU_SCROLL_INTERVAL 350

String menuNames[MAX_MENU_ITEMS];
String menuUris[MAX_MENU_ITEMS];
int menuItemCount   = 0;
int menuSelectedIdx = 0;
int menuScrollOffset = 0;
int menuScrollIdx    = 0;
unsigned long menuEnteredTime = 0;
unsigned long lastMenuScrollTime = 0;
const unsigned long MENU_TIMEOUT_MS = 15000; // 15 δευτ. auto-exit

String currentContextUri = "";

// ================= Χρονισμοί δικτύου (για να μην παγώνει η οθόνη) =================
const unsigned long SPOTIFY_POLL_MS       = 3500;   // polling στην κανονική λειτουργία
const unsigned long SPOTIFY_POLL_SAVER_MS = 10000;  // polling όταν δείχνει το ρολόι (screensaver)
const int  HTTP_TIMEOUT_MS = 2500;                  // μέγιστη αναμονή ανά αίτημα (το default ήταν 5000)
unsigned long lastTokenAttempt = 0;                 // για retry/backoff στο refresh του token

void setShortTimeouts(HTTPClient& h) {
  h.setConnectTimeout(HTTP_TIMEOUT_MS);
  h.setTimeout(HTTP_TIMEOUT_MS);
}

// ================= 5. CALLBACK ΟΘΟΝΗΣ ΓΙΑ JPEG ========
bool tft_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
  if (inScreensaver || y >= tft.height()) return 0;
  tft.drawRGBBitmap(x, y, bitmap, w, h);
  return 1;
}

// ================= 6. INTERRUPT ROTARY ENCODER ========
void IRAM_ATTR readEncoderISR() {
  unsigned long now = millis();
  if (now - lastEncISR > 12) { // 12ms Debounce για ακριβώς 1 click ανά βήμα
    if (digitalRead(ENC_DT) == HIGH) {
      encoderDelta++;
    } else {
      encoderDelta--;
    }
    lastEncISR = now;
  }
}

// ================= 7. ΒΟΗΘΗΤΙΚΕΣ ΣΥΝΑΡΤΗΣΕΙΣ UI =======
void updateCurrentSong();
void initMainUI();
void updateTrackText();
void drawScreensaverUI(bool forceRedraw = false);
void enterScreensaver();
void exitScreensaver();
void fetchWeatherAndLocation();
void enterMainMenu();
void exitMenu();
void menuSelect();
void menuNavigate(int direction);
void updateMenuScroll();

const char* getWeatherDesc(int code) {
  switch (code) {
    case 0: return "Clear Sky";
    case 1: return "Mainly Clear";
    case 2: return "Partly Cloudy";
    case 3: return "Overcast";
    case 45: case 48: return "Foggy";
    case 51: case 53: case 55: return "Light Drizzle";
    case 61: case 63: case 65: return "Rainy";
    case 71: case 73: case 75: return "Snowy";
    case 80: case 81: case 82: return "Rain Showers";
    case 95: case 96: case 99: return "Thunderstorm";
    default: return "Partly Cloudy";
  }
}

void fetchWeatherAndLocation() {
  if (WiFi.status() != WL_CONNECTED) return;
  WiFiClient client;
  HTTPClient http;

  static bool locationFetched = false;
  if (!locationFetched) {
    // 1. Αν ο χρήστης έχει ορίσει πόλη στο CITY_NAME (Open-Meteo Geocoding)
    if (strlen(CITY_NAME) > 0) {
      String geoUrl = "http://geocoding-api.open-meteo.com/v1/search?name=" + String(CITY_NAME) + "&count=1";
      if (http.begin(client, geoUrl)) {
        setShortTimeouts(http);
        int httpCode = http.GET();
        if (httpCode == 200) {
          String payload = http.getString();
          JsonDocument doc;
          DeserializationError err = deserializeJson(doc, payload);
          if (!err && doc["results"].is<JsonArray>() && doc["results"].size() > 0) {
            userCity = doc["results"][0]["name"].as<String>();
            userLat  = doc["results"][0]["latitude"].as<float>();
            userLon  = doc["results"][0]["longitude"].as<float>();
            locationFetched = true;
            Serial.printf("[Weather] Geocoding %s -> Lat: %.4f, Lon: %.4f\n", userCity.c_str(), userLat, userLon);
          }
        }
        http.end();
      }
    }

    // 2. Fallback: Αυτόματος εντοπισμός μέσω IP (ip-api.com)
    if (!locationFetched) {
      if (http.begin(client, "http://ip-api.com/json/")) {
        setShortTimeouts(http);
        int httpCode = http.GET();
        if (httpCode == 200) {
          String payload = http.getString();
          JsonDocument doc;
          DeserializationError err = deserializeJson(doc, payload);
          if (!err && !doc["city"].isNull()) {
            userCity = doc["city"].as<String>();
            userLat  = doc["lat"].as<float>();
            userLon  = doc["lon"].as<float>();
            locationFetched = true;
            Serial.printf("[Weather] IP Geolocation -> %s (Lat: %.4f, Lon: %.4f)\n", userCity.c_str(), userLat, userLon);
          }
        }
        http.end();
      }
    }
  }

  // 3. Πρόγνωση καιρού από Open-Meteo (current_weather=true)
  String weatherUrl = "http://api.open-meteo.com/v1/forecast?latitude=" + String(userLat, 4) + 
                      "&longitude=" + String(userLon, 4) + 
                      "&current_weather=true";
  if (http.begin(client, weatherUrl)) {
    setShortTimeouts(http);
    int httpCode = http.GET();
    if (httpCode == 200) {
      String payload = http.getString();
      JsonDocument doc;
      DeserializationError err = deserializeJson(doc, payload);
      if (!err && !doc["current_weather"].isNull()) {
        currentTemp = doc["current_weather"]["temperature"].as<float>();
        int wCode   = doc["current_weather"]["weathercode"].as<int>();
        currentWeatherDesc = getWeatherDesc(wCode);
        hasWeatherData = true;
        Serial.printf("[Weather] %s: %.1f C, %s\n", userCity.c_str(), currentTemp, currentWeatherDesc.c_str());
      }
    }
    http.end();
  }
  lastWeatherUpdate = millis();
}

void drawScreensaverUI(bool forceRedraw) {
  if (forceRedraw) {
    tft.fillScreen(UI_BLACK);

    // Καντράν αναλογικού ρολογιού (Κύκλος & 12 δείκτες ωρών)
    tft.drawCircle(CLOCK_X, CLOCK_Y, CLOCK_R, UI_GRAY);
    for (int i = 0; i < 12; i++) {
      float a = i * 30.0 * 0.0174532925;
      int x1 = CLOCK_X + round(CLOCK_R * sin(a));
      int y1 = CLOCK_Y - round(CLOCK_R * cos(a));
      int len = (i % 3 == 0) ? 4 : 2;
      int x2 = CLOCK_X + round((CLOCK_R - len) * sin(a));
      int y2 = CLOCK_Y - round((CLOCK_R - len) * cos(a));
      tft.drawLine(x1, y1, x2, y2, (i % 3 == 0) ? UI_WHITE : UI_GRAY);
    }

    // Διαχωριστικές γραμμές
    tft.drawFastHLine(25, 84, 110, UI_GRAY);
    tft.drawFastHLine(35, 114, 90, UI_GRAY);

    // Οδηγία επαναφοράς
    tft.setTextColor(UI_GRAY, UI_BLACK);
    tft.setTextSize(1);
    tft.setCursor(26, 118);
    tft.print("Press PLAY to wake");

    lastClockDateStr    = "";
    lastClockLocStr     = "";
    lastClockWeatherStr = "";
    lastSecX  = -1; lastSecY  = -1;
    lastMinX  = -1; lastMinY  = -1;
    lastHourX = -1; lastHourY = -1;
  }

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 50)) {
    return;
  }

  // 1. Αναλογικό Ρολόι - Υπολογισμός Δεικτών
  float sAngle = timeinfo.tm_sec * 6.0 * 0.0174532925;
  float mAngle = (timeinfo.tm_min + timeinfo.tm_sec / 60.0) * 6.0 * 0.0174532925;
  float hAngle = ((timeinfo.tm_hour % 12) + timeinfo.tm_min / 60.0) * 30.0 * 0.0174532925;

  int newSecX  = CLOCK_X + round(S_LEN * sin(sAngle));
  int newSecY  = CLOCK_Y - round(S_LEN * cos(sAngle));
  int newMinX  = CLOCK_X + round(M_LEN * sin(mAngle));
  int newMinY  = CLOCK_Y - round(M_LEN * cos(mAngle));
  int newHourX = CLOCK_X + round(H_LEN * sin(hAngle));
  int newHourY = CLOCK_Y - round(H_LEN * cos(hAngle));

  // Σβήσιμο προηγούμενων δεικτών (αν άλλαξαν θέση)
  if (lastSecX != -1 && (newSecX != lastSecX || newSecY != lastSecY)) {
    tft.drawLine(CLOCK_X, CLOCK_Y, lastSecX, lastSecY, UI_BLACK);
  }
  if (lastMinX != -1 && (newMinX != lastMinX || newMinY != lastMinY)) {
    tft.drawLine(CLOCK_X, CLOCK_Y, lastMinX, lastMinY, UI_BLACK);
  }
  if (lastHourX != -1 && (newHourX != lastHourX || newHourY != lastHourY)) {
    tft.drawLine(CLOCK_X, CLOCK_Y, lastHourX, lastHourY, UI_BLACK);
  }

  // Σχεδίαση νέων δεικτών
  tft.drawLine(CLOCK_X, CLOCK_Y, newHourX, newHourY, UI_WHITE);
  tft.drawLine(CLOCK_X, CLOCK_Y, newMinX, newMinY, UI_CYAN);
  tft.drawLine(CLOCK_X, CLOCK_Y, newSecX, newSecY, UI_GREEN);
  tft.fillCircle(CLOCK_X, CLOCK_Y, 2, UI_WHITE);

  lastSecX  = newSecX;  lastSecY  = newSecY;
  lastMinX  = newMinX;  lastMinY  = newMinY;
  lastHourX = newHourX; lastHourY = newHourY;

  // 2. Ημερομηνία κάτω από το ρολόι (π.χ. "Sat, 31 Oct 2026")
  char dateBuf[25];
  strftime(dateBuf, sizeof(dateBuf), "%a, %d %b %Y", &timeinfo);
  String currentDateStr = String(dateBuf);
  if (currentDateStr != lastClockDateStr || forceRedraw) {
    lastClockDateStr = currentDateStr;
    tft.setTextSize(1);
    tft.setTextColor(UI_WHITE, UI_BLACK);
    int dateX = (160 - (currentDateStr.length() * 6)) / 2;
    if (dateX < 0) dateX = 0;
    tft.fillRect(10, 72, 140, 10, UI_BLACK);
    tft.setCursor(dateX, 72);
    tft.print(currentDateStr);
  }

  // 3. Τοποθεσία & Θερμοκρασία (π.χ. "Thessaloniki  17  C")
  char locBuf[30];
  snprintf(locBuf, sizeof(locBuf), "%s  %.0f  C", userCity.c_str(), currentTemp);
  String currentLocStr = String(locBuf);
  if (currentLocStr != lastClockLocStr || forceRedraw) {
    lastClockLocStr = currentLocStr;
    tft.setTextSize(1);
    tft.setTextColor(UI_GREEN, UI_BLACK);
    int locX = (160 - (currentLocStr.length() * 6)) / 2;
    if (locX < 0) locX = 0;
    tft.fillRect(10, 88, 140, 10, UI_BLACK);
    tft.setCursor(locX, 88);
    tft.print(currentLocStr);
    // Σύμβολο βαθμών Κελσίου (κύκλος μετατοπισμένος προς τα δεξιά ανάμεσα στον αριθμό και το C)
    int degX = locX + (currentLocStr.length() - 2) * 6 + 2;
    tft.drawCircle(degX, 89, 1, UI_GREEN);
  }

  // 4. Περιγραφή Καιρού (π.χ. "Clear Sky", "Light Drizzle")
  if (currentWeatherDesc != lastClockWeatherStr || forceRedraw) {
    lastClockWeatherStr = currentWeatherDesc;
    tft.setTextSize(1);
    tft.setTextColor(UI_CYAN, UI_BLACK);
    int wX = (160 - (currentWeatherDesc.length() * 6)) / 2;
    if (wX < 0) wX = 0;
    tft.fillRect(10, 100, 140, 10, UI_BLACK);
    tft.setCursor(wX, 100);
    tft.print(currentWeatherDesc);
  }
}

void enterScreensaver() {
  inScreensaver = true;
  tft.fillScreen(UI_BLACK);
  drawScreensaverUI(true);
}

void exitScreensaver() {
  inScreensaver = false;
  lastPlaybackActiveTime = millis();
  initMainUI();
  currentTrackId = ""; // Αναγκάζει πλήρη επανασχεδίαση τίτλου & εξωφύλλου
  lastProgressWidth = -1;
  lastDisplayedTime = "";
  updateCurrentSong();
}

void showLoadingScreen(const char* msg) {
  tft.fillScreen(UI_BLACK);
  tft.setTextColor(UI_WHITE, UI_BLACK);
  tft.setTextSize(1);
  int x = (160 - (strlen(msg) * 6)) / 2;
  tft.setCursor(x > 0 ? x : 10, 60);
  tft.print(msg);
}

void initMainUI() {
  tft.fillScreen(UI_BLACK);
  tft.drawRect(6, 10, 64, 64, UI_GRAY);       // Πλαίσιο εξωφύλλου 64x64
  tft.drawFastHLine(0, 86, 160, UI_GRAY);     // Διαχωριστική γραμμή
  tft.drawFastHLine(6, 116, 148, UI_GRAY);    // Βάση μπάρας προόδου
}

long getCurrentProgress() {
  if (!hasValidData || trackDuration <= 0) return 0;
  if (!isPlaying) return serverProgress;
  long calc = serverProgress + (millis() - lastServerUpdate);
  if (calc > trackDuration) calc = trackDuration;
  if (calc < 0) calc = 0;
  return calc;
}

// Δημιουργεί το τμήμα κειμένου 13 χαρακτήρων για κυκλική κύλιση (Marquee)
String getScrolledSlice(const String& text, int& scrollIdx) {
  if ((int)text.length() <= MAX_CHARS) {
    return text;
  }
  String loopStr = text + SCROLL_SEP;
  int len = loopStr.length();
  String result = "";
  for (int i = 0; i < MAX_CHARS; i++) {
    result += loopStr[(scrollIdx + i) % len];
  }
  scrollIdx = (scrollIdx + 1) % len;
  return result;
}

void updateStatusRow() {
  if (inScreensaver) return;
  tft.setTextSize(1);
  tft.fillRect(76, 62, 82, 10, UI_BLACK);

  // Play / Pause + Shuffle + Repeat
  tft.setTextColor(UI_GREEN, UI_BLACK);
  tft.setCursor(76, 62);
  tft.print(isPlaying ? ">" : "||");

  if (shuffleState) {
    tft.print(" S");
  }
  if (repeatState == "context") {
    tft.print(" R");
  } else if (repeatState == "track") {
    tft.print(" R1");
  }

  // Ένταση ήχου
  tft.setTextColor(UI_WHITE, UI_BLACK);
  tft.setCursor(116, 62);
  tft.printf("V:%d%%", currentVolume);
}

// Καλείται όταν αλλάζει το τραγούδι για αρχικό καθαρισμό και σχεδίαση
void updateTrackText() {
  if (inScreensaver) return;
  tft.fillRect(76, 10, 84, 48, UI_BLACK);
  tft.setTextSize(1);

  scrollIdxTitle   = 0;
  scrollIdxArtist  = 0;
  scrollIdxAlbum   = 0;
  lastScrollUpdate = millis();

  // 1. Τίτλος Τραγουδιού (Λευκό)
  tft.setTextColor(UI_WHITE, UI_BLACK);
  tft.setCursor(76, 12);
  tft.print(getScrolledSlice(currentTitle, scrollIdxTitle));

  // 2. Καλλιτέχνης (Ανοιχτό Γκρι)
  tft.setTextColor(UI_LIGHT_GRAY, UI_BLACK);
  tft.setCursor(76, 28);
  tft.print(getScrolledSlice(currentArtist, scrollIdxArtist));

  // 3. Όνομα Άλμπουμ (Κυανό)
  tft.setTextColor(UI_CYAN, UI_BLACK);
  tft.setCursor(76, 44);
  tft.print(getScrolledSlice(currentAlbum, scrollIdxAlbum));

  updateStatusRow();
}

// Καλείται κάθε SCROLL_INTERVAL (600ms) και κυλάει τον τίτλο, τον τραγουδιστή και το άλμπουμ
void updateScrollingText() {
  if (inScreensaver || !hasValidData) return;
  tft.setTextSize(1);

  if ((int)currentTitle.length() > MAX_CHARS) {
    tft.setTextColor(UI_WHITE, UI_BLACK);
    tft.setCursor(76, 12);
    tft.print(getScrolledSlice(currentTitle, scrollIdxTitle));
  }

  if ((int)currentArtist.length() > MAX_CHARS) {
    tft.setTextColor(UI_LIGHT_GRAY, UI_BLACK);
    tft.setCursor(76, 28);
    tft.print(getScrolledSlice(currentArtist, scrollIdxArtist));
  }

  if ((int)currentAlbum.length() > MAX_CHARS) {
    tft.setTextColor(UI_CYAN, UI_BLACK);
    tft.setCursor(76, 44);
    tft.print(getScrolledSlice(currentAlbum, scrollIdxAlbum));
  }
}

void updateProgressBarAndTime() {
  if (inScreensaver || trackDuration <= 0) return;
  long cur = getCurrentProgress();

  // Μπάρα προόδου (μήκος 148px)
  int w = map(cur, 0, trackDuration, 0, 148);
  w = constrain(w, 0, 148);
  if (w != lastProgressWidth) {
    lastProgressWidth = w;
    tft.drawFastHLine(6, 116, 148, UI_GRAY);
    tft.drawFastHLine(6, 117, 148, UI_GRAY);
    if (w > 0) {
      tft.drawFastHLine(6, 116, w, UI_GREEN);
      tft.drawFastHLine(6, 117, w, UI_GREEN);
    }
  }

  // Χρονόμετρο ΛΛ:ΔΔ
  int pMin = cur / 60000;
  int pSec = (cur % 60000) / 1000;
  int dMin = trackDuration / 60000;
  int dSec = (trackDuration % 60000) / 1000;

  char timeBuf[20];
  snprintf(timeBuf, sizeof(timeBuf), "%d:%02d/%d:%02d", pMin, pSec, dMin, dSec);
  String timeStr = String(timeBuf);

  if (timeStr != lastDisplayedTime) {
    lastDisplayedTime = timeStr;
    tft.setTextColor(UI_LIGHT_GRAY, UI_BLACK);
    tft.setTextSize(1);

    tft.fillRect(6, 96, 36, 10, UI_BLACK);
    tft.setCursor(6, 96);
    tft.printf("%d:%02d", pMin, pSec);

    tft.fillRect(124, 96, 32, 10, UI_BLACK);
    tft.setCursor(124, 96);
    tft.printf("%d:%02d", dMin, dSec);
  }
}

void drawAlbumArtFromUrl(String url) {
  if (inScreensaver || url == "") return;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;

  if (https.begin(client, url)) {
    int httpCode = https.GET();
    if (httpCode == HTTP_CODE_OK) {
      int totalLen = https.getSize();
      if (totalLen > 0 && totalLen < (int)sizeof(jpgBuffer)) {
        WiFiClient* stream = https.getStreamPtr();
        int bytesRead = 0;
        while (https.connected() && bytesRead < totalLen) {
          size_t avail = stream->available();
          if (avail) {
            int r = stream->readBytes(jpgBuffer + bytesRead, avail);
            bytesRead += r;
          }
          yield();
        }
        tft.fillRect(6, 10, 64, 64, UI_BLACK);
        TJpgDec.drawJpg(6, 10, jpgBuffer, bytesRead);
      }
    }
    https.end();
  }
}

// ================= 8. SPOTIFY API ΚΛΗΣΕΙΣ =============
bool refreshSpotifyToken() {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;

  if (!https.begin(client, "https://accounts.spotify.com/api/token")) return false;
  setShortTimeouts(https);

  String auth = base64::encode(String(CLIENT_ID) + ":" + String(CLIENT_SECRET));
  https.addHeader("Authorization", "Basic " + auth);
  https.addHeader("Content-Type", "application/x-www-form-urlencoded");

  String body = "grant_type=refresh_token&refresh_token=" + String(REFRESH_TOKEN);
  int httpCode = https.POST(body);

  if (httpCode == 200) {
    JsonDocument doc;
    deserializeJson(doc, https.getString());
    accessToken = doc["access_token"].as<String>();
    lastTokenTime = millis();
    https.end();
    return true;
  }
  https.end();
  return false;
}

void sendSpotifyCommand(const char* method, String endpoint) {
  if (accessToken == "" || WiFi.status() != WL_CONNECTED) return;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;

  String url = "https://api.spotify.com/v1/me/player" + endpoint;
  https.begin(client, url);
  https.addHeader("Authorization", "Bearer " + accessToken);
  https.addHeader("Content-Length", "0");

  if (strcmp(method, "POST") == 0) {
    https.POST("");
  } else if (strcmp(method, "PUT") == 0) {
    https.PUT("");
  }
  https.end();
  lastServerUpdate = millis();
}

void sendSpotifyPUT(String endpoint, String jsonBody) {
  if (accessToken == "" || WiFi.status() != WL_CONNECTED) return;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  String url = "https://api.spotify.com/v1/me/player" + endpoint;
  https.begin(client, url);
  https.addHeader("Authorization", "Bearer " + accessToken);
  https.addHeader("Content-Type", "application/json");
  int code = https.PUT(jsonBody);
  Serial.printf("[Spotify] PUT %s returned %d\n", endpoint.c_str(), code);
  https.end();
  lastServerUpdate = millis();
}

// ================= 8b. MENU MODE (Queue & Playlists) ====
void drawMenuUI() {
  tft.fillScreen(UI_BLACK);

  // Τίτλος μενού
  const char* title;
  switch (menuState) {
    case MENU_MAIN:      title = "MENU"; break;
    case MENU_QUEUE:     title = "QUEUE - Next Up"; break;
    case MENU_PLAYLISTS: title = "PLAYLISTS"; break;
    default:             title = "MENU"; break;
  }
  tft.setTextColor(UI_GREEN, UI_BLACK);
  tft.setTextSize(1);
  int tX = (160 - (strlen(title) * 6)) / 2;
  tft.setCursor(tX > 0 ? tX : 2, 4);
  tft.print(title);

  tft.drawFastHLine(4, 16, 152, UI_GRAY);

  // Λίστα αντικειμένων
  int maxVisible = (menuItemCount < MENU_VISIBLE) ? menuItemCount : MENU_VISIBLE;
  for (int i = 0; i < maxVisible; i++) {
    int dataIdx = menuScrollOffset + i;
    if (dataIdx >= menuItemCount) break;
    int y = 22 + i * 16;

    if (dataIdx == menuSelectedIdx) {
      tft.setTextColor(UI_GREEN, UI_BLACK);
      tft.setCursor(4, y);
      tft.print(">");
      tft.setTextColor(UI_WHITE, UI_BLACK);
    } else {
      tft.setTextColor(UI_LIGHT_GRAY, UI_BLACK);
    }
    tft.setCursor(14, y);
    String display = menuNames[dataIdx];
    if (dataIdx == menuSelectedIdx) {
      if ((int)display.length() > MENU_MAX_CHARS) {
        display = display.substring(0, MENU_MAX_CHARS);
      }
    } else {
      if ((int)display.length() > MENU_MAX_CHARS) {
        display = display.substring(0, MENU_MAX_CHARS - 2) + "..";
      }
    }
    tft.print(display);
  }

  if (menuItemCount == 0) {
    tft.setTextColor(UI_GRAY, UI_BLACK);
    tft.setCursor(40, 50);
    tft.print("(No items)");
  }

  // Scrollbar indicator (αν υπάρχουν > 5 αντικείμενα)
  if (menuItemCount > MENU_VISIBLE) {
    int barH = 60 * MENU_VISIBLE / menuItemCount;
    if (barH < 4) barH = 4;
    int barY = 22 + (60 - barH) * menuScrollOffset / (menuItemCount - MENU_VISIBLE);
    tft.drawFastVLine(156, 22, 60, UI_GRAY);
    tft.fillRect(155, barY, 3, barH, UI_GREEN);
  }

  // Οδηγίες
  tft.drawFastHLine(4, 104, 152, UI_GRAY);
  tft.setTextColor(UI_GRAY, UI_BLACK);
  tft.setTextSize(1);
  if (menuState == MENU_MAIN) {
    tft.setCursor(14, 110);
    tft.print("Rotate + Click=Select");
  } else {
    tft.setCursor(8, 110);
    tft.print("Click=Play PLAY=Back");
  }

  menuScrollIdx = 0;
  lastMenuScrollTime = millis();
}

// Κύλιση κειμένου (Marquee) για το επιλεγμένο στοιχείο του Menu
void updateMenuScroll() {
  if (menuState == MENU_NONE || menuItemCount == 0) return;
  if (menuSelectedIdx < 0 || menuSelectedIdx >= menuItemCount) return;

  String fullText = menuNames[menuSelectedIdx];
  if ((int)fullText.length() <= MENU_MAX_CHARS) return;

  int visibleRow = menuSelectedIdx - menuScrollOffset;
  if (visibleRow < 0 || visibleRow >= MENU_VISIBLE) return;

  int y = 22 + visibleRow * 16;
  tft.fillRect(14, y, 140, 10, UI_BLACK);
  tft.setTextColor(UI_WHITE, UI_BLACK);
  tft.setTextSize(1);
  tft.setCursor(14, y);

  String loopStr = fullText + "    ";
  int len = loopStr.length();
  String slice = "";
  for (int i = 0; i < MENU_MAX_CHARS; i++) {
    slice += loopStr[(menuScrollIdx + i) % len];
  }
  menuScrollIdx = (menuScrollIdx + 1) % len;
  tft.print(slice);
}

void fetchQueue() {
  menuItemCount = 0;
  if (accessToken == "" || WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;

  if (!https.begin(client, "https://api.spotify.com/v1/me/player/queue")) return;
  https.addHeader("Authorization", "Bearer " + accessToken);

  int httpCode = https.GET();
  Serial.printf("[Spotify] Queue HTTP: %d\n", httpCode);

  if (httpCode == 200) {
    String payload = https.getString();
    JsonDocument filter;
    filter["queue"][0]["name"] = true;
    filter["queue"][0]["uri"] = true;
    filter["queue"][0]["artists"][0]["name"] = true;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload, DeserializationOption::Filter(filter));
    if (!err && doc["queue"].is<JsonArray>()) {
      JsonArray queue = doc["queue"].as<JsonArray>();
      int count = 0;
      for (JsonObject track : queue) {
        if (count >= MAX_MENU_ITEMS) break;
        String name = track["name"].as<String>();
        String artist = "";
        if (track["artists"].is<JsonArray>() && track["artists"].size() > 0) {
          artist = track["artists"][0]["name"].as<String>();
        }
        menuNames[count] = (artist.length() > 0) ? (artist + " - " + name) : name;
        menuUris[count]  = track["uri"].as<String>();
        count++;
      }
      menuItemCount = count;
      Serial.printf("[Spotify] Loaded %d queue tracks\n", count);
    } else {
      Serial.printf("[Spotify] Queue JSON error: %s\n", err.c_str());
    }
  } else if (httpCode == 403) {
    menuNames[0] = "Scope Error: Token";
    menuNames[1] = "Needs playback scope";
    menuUris[0]  = "";
    menuUris[1]  = "";
    menuItemCount = 2;
  }
  https.end();
}

void fetchPlaylists() {
  menuItemCount = 0;
  if (accessToken == "" || WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;

  if (!https.begin(client, "https://api.spotify.com/v1/me/playlists?limit=25")) return;
  https.addHeader("Authorization", "Bearer " + accessToken);

  int httpCode = https.GET();
  Serial.printf("[Spotify] Playlists HTTP: %d\n", httpCode);

  if (httpCode == 200) {
    String payload = https.getString();
    JsonDocument filter;
    filter["items"][0]["name"] = true;
    filter["items"][0]["uri"] = true;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload, DeserializationOption::Filter(filter));
    if (!err && doc["items"].is<JsonArray>()) {
      JsonArray items = doc["items"].as<JsonArray>();
      int count = 0;
      for (JsonObject pl : items) {
        if (count >= MAX_MENU_ITEMS) break;
        String name = pl["name"].as<String>();
        String uri  = pl["uri"].as<String>();
        if (name.length() > 0 && uri.length() > 0) {
          menuNames[count] = name;
          menuUris[count]  = uri;
          count++;
        }
      }
      menuItemCount = count;
      Serial.printf("[Spotify] Loaded %d playlists\n", count);
    } else {
      Serial.printf("[Spotify] Playlists JSON error: %s\n", err.c_str());
    }
  } else if (httpCode == 403) {
    menuNames[0] = "Scope Error: Token";
    menuNames[1] = "Needs playlist scope";
    menuUris[0]  = "";
    menuUris[1]  = "";
    menuItemCount = 2;
  }
  https.end();
}

void enterMainMenu() {
  menuState = MENU_MAIN;
  menuItemCount = 2;
  menuNames[0] = "Queue (Next Up)";
  menuNames[1] = "Playlists";
  menuUris[0] = "";
  menuUris[1] = "";
  menuSelectedIdx  = 0;
  menuScrollOffset = 0;
  menuScrollIdx    = 0;
  menuEnteredTime  = millis();
  lastMenuScrollTime = millis();
  drawMenuUI();
}

void enterQueueMenu() {
  menuState = MENU_QUEUE;
  menuSelectedIdx  = 0;
  menuScrollOffset = 0;
  menuScrollIdx    = 0;
  tft.fillScreen(UI_BLACK);
  tft.setTextColor(UI_WHITE, UI_BLACK);
  tft.setTextSize(1);
  tft.setCursor(30, 56);
  tft.print("Loading Queue...");
  fetchQueue();
  menuEnteredTime = millis();
  lastMenuScrollTime = millis();
  drawMenuUI();
}

void enterPlaylistMenu() {
  menuState = MENU_PLAYLISTS;
  menuSelectedIdx  = 0;
  menuScrollOffset = 0;
  menuScrollIdx    = 0;
  tft.fillScreen(UI_BLACK);
  tft.setTextColor(UI_WHITE, UI_BLACK);
  tft.setTextSize(1);
  tft.setCursor(20, 56);
  tft.print("Loading Playlists...");
  fetchPlaylists();
  menuEnteredTime = millis();
  lastMenuScrollTime = millis();
  drawMenuUI();
}

void exitMenu() {
  menuState = MENU_NONE;
  lastPlaybackActiveTime = millis();
  initMainUI();
  currentTrackId = "";
  lastProgressWidth = -1;
  lastDisplayedTime = "";
  updateCurrentSong();
}

void menuSelect() {
  if (menuItemCount == 0) return;
  menuEnteredTime = millis();

  if (menuState == MENU_MAIN) {
    if (menuSelectedIdx == 0) {
      enterQueueMenu();
    } else {
      enterPlaylistMenu();
    }
  } else if (menuState == MENU_QUEUE) {
    // Αναπαραγωγή του επιλεγμένου τραγουδιού από την ουρά
    String uri = menuUris[menuSelectedIdx];
    if (uri.length() > 0) {
      if (currentContextUri.length() > 0) {
        // Μεταπήδηση στο τραγούδι εντός του υπάρχοντος playlist/album
        // χωρίς να καθαριστεί η ουρά!
        String body = "{\"context_uri\":\"" + currentContextUri + "\",\"offset\":{\"uri\":\"" + uri + "\"}}";
        sendSpotifyPUT("/play", body);
      } else {
        // Αν δεν υπάρχει context (π.χ. Liked Songs), εκτελούμε skips
        for (int s = 0; s <= menuSelectedIdx; s++) {
          sendSpotifyCommand("POST", "/next");
          if (s < menuSelectedIdx) delay(120);
        }
      }
      delay(400);
      exitMenu();
    }
  } else if (menuState == MENU_PLAYLISTS) {
    // Αναπαραγωγή ολόκληρης της λίστας αναπαραγωγής
    String uri = menuUris[menuSelectedIdx];
    if (uri.length() > 0) {
      sendSpotifyPUT("/play", "{\"context_uri\":\"" + uri + "\"}");
      currentContextUri = uri; // Ενημέρωση του ενεργού context
      delay(400);
      exitMenu();
    }
  }
}

void menuNavigate(int direction) {
  if (menuItemCount == 0) return;
  menuEnteredTime = millis();
  menuScrollIdx = 0;
  lastMenuScrollTime = millis();

  menuSelectedIdx += direction;
  if (menuSelectedIdx < 0) menuSelectedIdx = menuItemCount - 1;
  if (menuSelectedIdx >= menuItemCount) menuSelectedIdx = 0;

  // Scroll αν ο δείκτης βγήκε εκτός ορατής περιοχής
  if (menuSelectedIdx < menuScrollOffset) {
    menuScrollOffset = menuSelectedIdx;
  }
  if (menuSelectedIdx >= menuScrollOffset + MENU_VISIBLE) {
    menuScrollOffset = menuSelectedIdx - MENU_VISIBLE + 1;
  }
  drawMenuUI();
}

void updateCurrentSong() {
  if (accessToken == "" || WiFi.status() != WL_CONNECTED) return;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;

  if (!https.begin(client, "https://api.spotify.com/v1/me/player")) return;
  setShortTimeouts(https);
  https.addHeader("Authorization", "Bearer " + accessToken);

  int httpCode = https.GET();
  if (httpCode == 200) {
    JsonDocument filter;
    filter["is_playing"] = true;
    filter["shuffle_state"] = true;
    filter["repeat_state"] = true;
    filter["progress_ms"] = true;
    filter["context"]["uri"] = true;
    filter["device"]["volume_percent"] = true;
    filter["item"]["id"] = true;
    filter["item"]["name"] = true;
    filter["item"]["duration_ms"] = true;
    filter["item"]["album"]["name"] = true;
    filter["item"]["album"]["images"] = true;
    filter["item"]["artists"][0]["name"] = true;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, https.getStream(), DeserializationOption::Filter(filter));

    if (!err && !doc["item"].isNull()) {
      hasValidData = true;
      if (!doc["context"].isNull() && !doc["context"]["uri"].isNull()) {
        currentContextUri = doc["context"]["uri"].as<String>();
      } else {
        currentContextUri = "";
      }
      String newTrackId = doc["item"]["id"].as<String>();
      bool newPlaying   = doc["is_playing"].as<bool>();
      bool newShuffle   = doc["shuffle_state"].as<bool>();
      String newRepeat  = doc["repeat_state"].isNull() ? "off" : doc["repeat_state"].as<String>();
      serverProgress    = doc["progress_ms"].as<long>();
      trackDuration     = doc["item"]["duration_ms"].as<long>();
      lastServerUpdate  = millis();

      if (!volumePendingSend && !doc["device"]["volume_percent"].isNull()) {
        currentVolume = doc["device"]["volume_percent"].as<int>();
      }

      bool statusChanged = (newPlaying != isPlaying) || (newShuffle != shuffleState) || (newRepeat != repeatState);
      isPlaying    = newPlaying;
      shuffleState = newShuffle;
      repeatState  = newRepeat;

      if (isPlaying) {
        lastPlaybackActiveTime = millis();
        if (inScreensaver) {
          inScreensaver = false;
          initMainUI();
          currentTrackId = "";
          lastProgressWidth = -1;
          lastDisplayedTime = "";
        }
      }

      if (!inScreensaver) {
        if (newTrackId != currentTrackId) {
          currentTrackId = newTrackId;
          currentTitle   = doc["item"]["name"].as<String>();
          currentArtist  = doc["item"]["artists"][0]["name"].as<String>();
          currentAlbum   = doc["item"]["album"]["name"].as<String>();

          updateTrackText();

          // Εύρεση του 64x64 εξωφύλλου
          JsonArray images = doc["item"]["album"]["images"].as<JsonArray>();
          String imgUrl = "";
          for (JsonObject img : images) {
            if (img["height"] == 64) {
              imgUrl = img["url"].as<String>();
              break;
            }
          }
          if (imgUrl == "" && images.size() > 0) {
            imgUrl = images[images.size() - 1]["url"].as<String>();
          }
          drawAlbumArtFromUrl(imgUrl);
        } else if (statusChanged) {
          updateStatusRow();
        }
      }
    }
  } else if (httpCode == 204) {
    if (isPlaying) {
      isPlaying = false;
      if (!inScreensaver) {
        updateStatusRow();
      }
    }
  }
  https.end();
}

// ================= 9. SETUP & LOOP ====================
void setup() {
  Serial.begin(115200);

  // Pins Κουμπιών & Rotary Encoder
  pinMode(ENC_CLK, INPUT_PULLUP);
  pinMode(ENC_DT, INPUT_PULLUP);
  pinMode(ENC_SW, INPUT_PULLUP);
  pinMode(BTN_PREV, INPUT_PULLUP);
  pinMode(BTN_PLAY, INPUT_PULLUP);
  pinMode(BTN_NEXT, INPUT_PULLUP);
  pinMode(BTN_SHUF, INPUT_PULLUP);

  attachInterrupt(digitalPinToInterrupt(ENC_CLK), readEncoderISR, FALLING);

  // Εκκίνηση Οθόνης ST7735 σε Οριζόντια Διάταξη (160x128)
  tft.initR(INITR_BLACKTAB);
  tft.setRotation(1);
  tft.setTextWrap(false);

  // Ρύθμιση JPEG Decoder
  TJpgDec.setJpgScale(1);
  TJpgDec.setSwapBytes(false);
  TJpgDec.setCallback(tft_output);

  showLoadingScreen("Connecting WiFi...");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
  }

  // Συγχρονισμός ώρας NTP (Ζώνη Ελλάδας: GMT+2 και αυτόματη θερινή ώρα)
  showLoadingScreen("Syncing Time...");
  configTzTime("EET-2EEST,M3.5.0/3,M10.5.0/4", "pool.ntp.org", "time.google.com");
  struct tm timeinfo;
  int retry = 0;
  while (!getLocalTime(&timeinfo, 500) && retry++ < 6) {
    delay(200);
  }

  // Ανάκτηση Τοποθεσίας (IP) & Καιρού (Open-Meteo)
  showLoadingScreen("Weather Info...");
  fetchWeatherAndLocation();

  showLoadingScreen("Spotify Auth...");
  refreshSpotifyToken();

  lastPlaybackActiveTime = millis();
  initMainUI();
  updateCurrentSong();
}

void loop() {
  unsigned long now = millis();

  // Έλεγχος αποσύνδεσης Wi-Fi
  if (WiFi.status() != WL_CONNECTED) {
    showLoadingScreen("Reconnecting WiFi...");
    WiFi.reconnect();
    unsigned long startAttempt = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 8000) {
      delay(500);
    }
    if (WiFi.status() == WL_CONNECTED) {
      if (inScreensaver) {
        drawScreensaverUI(true);
      } else {
        initMainUI();
        updateTrackText();
      }
    }
  }

  // 1. Ανανέωση Access Token κάθε 50 λεπτά.
  //    Αν αποτύχει, ξαναδοκιμάζει κάθε 15-60 s (πριν ξαναδοκίμαζε σε ΚΑΘΕ γύρο του loop
  //    και πάγωνε συνεχώς την οθόνη).
  bool tokenMissing = (accessToken == "");
  if ((tokenMissing || now - lastTokenTime > 3000000UL) &&
      now - lastTokenAttempt > (tokenMissing ? 15000UL : 60000UL)) {
    lastTokenAttempt = now;
    refreshSpotifyToken();
  }

  // 2. Ανάγνωση Rotary Encoder
  if (encoderDelta != 0) {
    noInterrupts();
    int step = encoderDelta;
    encoderDelta = 0;
    interrupts();

    if (menuState != MENU_NONE) {
      // Σε λειτουργία Menu: Πλοήγηση πάνω/κάτω
      menuNavigate(step > 0 ? 1 : -1);
      menuEnteredTime = millis();
    } else if (inScreensaver) {
      exitScreensaver();
    } else {
      // Κανονική λειτουργία: Ένταση ήχου
      currentVolume = constrain(currentVolume + (step * 2), 0, 100);
      lastVolumeChange = millis();
      volumePendingSend = true;
      lastPlaybackActiveTime = millis();
      updateStatusRow();
    }
  }

  if (volumePendingSend && (millis() - lastVolumeChange > 400)) {
    sendSpotifyCommand("PUT", "/volume?volume_percent=" + String(currentVolume));
    volumePendingSend = false;
  }

  // 3. Κουμπιά Ελέγχου (Active LOW)
  // Κουμπί K1 (BTN_PREV):
  //   Σύντομο πάτημα (<500ms): Προηγούμενο τραγούδι (ή Πίσω στο μενού)
  //   Παρατεταμένο πάτημα (>500ms): Επανεκκίνηση τρέχοντος τραγουδιού από την αρχή (Seek 0:00)
  if (digitalRead(BTN_PREV) == LOW) {
    lastPlaybackActiveTime = millis();
    if (menuState != MENU_NONE) {
      // Πίσω στο κύριο μενού ή έξοδος
      if (menuState == MENU_MAIN) {
        exitMenu();
      } else {
        enterMainMenu();
      }
      menuEnteredTime = millis();
      delay(250);
    } else if (inScreensaver) {
      exitScreensaver();
      delay(250);
    } else {
      // Κανονική αναπαραγωγή: Μέτρηση διάρκειας πατήματος
      unsigned long pressStart = millis();
      bool longPressTriggered = false;

      while (digitalRead(BTN_PREV) == LOW) {
        if (millis() - pressStart > 500 && !longPressTriggered) {
          longPressTriggered = true;
          // Παρατεταμένο πάτημα (>500ms): Επανεκκίνηση τρέχοντος τραγουδιού από την αρχή!
          sendSpotifyCommand("PUT", "/seek?position_ms=0");
          serverProgress = 0;
          lastServerUpdate = millis();
          lastProgressWidth = -1;
          lastDisplayedTime = "";
          if (!inScreensaver) updateProgressBarAndTime();
        }
        delay(10);
      }

      if (!longPressTriggered && (millis() - pressStart > 30)) {
        // Σύντομο πάτημα (<500ms): Προηγούμενο τραγούδι
        sendSpotifyCommand("POST", "/previous");
        delay(300);
        updateCurrentSong();
      }
      delay(200);
    }
  }

  if (digitalRead(BTN_PLAY) == LOW) {
    lastPlaybackActiveTime = millis();
    if (menuState != MENU_NONE) {
      exitMenu();
    } else if (inScreensaver) {
      exitScreensaver();
      isPlaying = true;
      sendSpotifyCommand("PUT", "/play");
    } else {
      isPlaying = !isPlaying;
      updateStatusRow();
      sendSpotifyCommand("PUT", isPlaying ? "/play" : "/pause");
    }
    delay(300);
  }

  if (digitalRead(BTN_NEXT) == LOW) {
    lastPlaybackActiveTime = millis();
    if (menuState != MENU_NONE) {
      // Σε Menu: Skip forward
      menuNavigate(1);
      menuEnteredTime = millis();
    } else if (inScreensaver) {
      exitScreensaver();
    } else {
      sendSpotifyCommand("POST", "/next");
      delay(300);
      updateCurrentSong();
    }
    delay(250);
  }

  // Κουμπί K4 (BTN_SHUF): Απλό πάτημα = Shuffle | Παρατεταμένο πάτημα (>500ms) = Repeat
  if (digitalRead(BTN_SHUF) == LOW && menuState == MENU_NONE) {
    if (inScreensaver) exitScreensaver();
    lastPlaybackActiveTime = millis();
    unsigned long pressStart = millis();
    bool longPressTriggered = false;

    while (digitalRead(BTN_SHUF) == LOW) {
      if (millis() - pressStart > 500 && !longPressTriggered) {
        longPressTriggered = true;
        if (repeatState == "off") {
          repeatState = "context";
        } else if (repeatState == "context") {
          repeatState = "track";
        } else {
          repeatState = "off";
        }
        if (!inScreensaver) updateStatusRow();
        sendSpotifyCommand("PUT", "/repeat?state=" + repeatState);
      }
      delay(10);
    }

    if (!longPressTriggered && (millis() - pressStart > 30)) {
      shuffleState = !shuffleState;
      if (!inScreensaver) updateStatusRow();
      sendSpotifyCommand("PUT", String("/shuffle?state=") + (shuffleState ? "true" : "false"));
    }
    delay(200);
  }

  // Κουμπί Rotary Encoder (ENC_SW):
  //   Σύντομο πάτημα (<700ms): Mute/Unmute (ή Επιλογή σε μενού)
  //   Παρατεταμένο πάτημα (>=700ms): Άνοιγμα / Κλείσιμο Μενού
  if (digitalRead(ENC_SW) == LOW) {
    unsigned long pressStart = millis();

    // Περιμένουμε μέχρι να αφήσει το κουμπί (ή μέχρι max 3 δευτερόλεπτα)
    while (digitalRead(ENC_SW) == LOW && (millis() - pressStart < 3000)) {
      delay(10);
    }
    unsigned long pressDuration = millis() - pressStart;
    delay(50); // Debounce μετά την απελευθέρωση

    if (pressDuration >= 700) {
      // Long press: Toggle Μενού
      if (inScreensaver) {
        exitScreensaver();
      } else if (menuState != MENU_NONE) {
        exitMenu();
      } else {
        enterMainMenu();
      }
    } else if (pressDuration > 40) {
      // Short press:
      if (inScreensaver) {
        exitScreensaver();
      } else if (menuState != MENU_NONE) {
        // Επιλογή στο μενού
        menuSelect();
      } else {
        // Mute / Unmute
        if (!isMuted) {
          savedMuteVolume = currentVolume;
          currentVolume = 0;
          isMuted = true;
        } else {
          currentVolume = savedMuteVolume;
          isMuted = false;
        }
        updateStatusRow();
        sendSpotifyCommand("PUT", "/volume?volume_percent=" + String(currentVolume));
      }
    }

    // Ανανέωση των χρονισμών αμέσως μετά την απελευθέρωση για αποφυγή underflow
    lastPlaybackActiveTime = millis();
    if (menuState != MENU_NONE) {
      menuEnteredTime = millis();
    }
    now = millis();
  }

  // 4. Διαχείριση UI Καταστάσεων
  if (menuState != MENU_NONE) {
    // Auto-exit μενού μετά από 15 δευτερόλεπτα αδράνειας
    if (millis() - menuEnteredTime >= MENU_TIMEOUT_MS) {
      exitMenu();
    } else if (now - lastMenuScrollTime >= MENU_SCROLL_INTERVAL) {
      lastMenuScrollTime = now;
      updateMenuScroll();
    }
  } else if (!inScreensaver) {
    // Είσοδος σε Screensaver αν περάσει ο χρόνος αδράνειας
    if (!isPlaying && (millis() - lastPlaybackActiveTime >= IDLE_TIMEOUT_MS)) {
      enterScreensaver();
      return;
    } else if (!hasValidData && (millis() - lastPlaybackActiveTime >= 10000)) {
      enterScreensaver();
      return;
    }

    // Ομαλή ανανέωση μπάρας & χρονομέτρου κάθε 250ms
    if (now - lastProgressUpdate >= 250 && hasValidData) {
      lastProgressUpdate = now;
      updateProgressBarAndTime();
    }

    // Αργή κύλιση τίτλου & τραγουδιστή κάθε 600ms (SCROLL_INTERVAL)
    if (now - lastScrollUpdate >= SCROLL_INTERVAL && hasValidData) {
      lastScrollUpdate = now;
      updateScrollingText();
    }
  } else {
    // Σε κατάσταση Screensaver: ξαναζωγραφίζουμε όταν αλλάξει το πραγματικό δευτερόλεπτο
    // του RTC (όχι με timer millis που ξεφεύγει από το δευτερόλεπτο και, μετά από
    // blocking κλήση δικτύου, δείχνει "κολλημένο" ρολόι και μετά άλμα).
    static int lastDrawnSec = -1;
    time_t tNow = time(nullptr);
    struct tm tmNow;
    localtime_r(&tNow, &tmNow);
    if (tmNow.tm_sec != lastDrawnSec) {
      lastDrawnSec = tmNow.tm_sec;
      drawScreensaverUI(false);
    }
  }

  // 5. Συγχρονισμός με το Spotify (μόνο εκτός μενού).
  //    Στο screensaver αραιότερα, και η μέτρηση ξεκινά από το ΤΕΛΟΣ της κλήσης,
  //    ώστε το loop να μη βρίσκεται σχεδόν συνέχεια μέσα σε blocking HTTPS.
  unsigned long pollInterval = inScreensaver ? SPOTIFY_POLL_SAVER_MS : SPOTIFY_POLL_MS;
  if (menuState == MENU_NONE && millis() - lastCheckTime >= pollInterval) {
    updateCurrentSong();
    lastCheckTime = millis();
  }

  // 6. Περιοδική ανανέωση καιρού κάθε 20 λεπτά
  if (now - lastWeatherUpdate >= 1200000UL) {
    fetchWeatherAndLocation();
  }
}