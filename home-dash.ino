/*
 * Home Dashboard - ESP32 BLE Sensor Monitor
 *  (dual-target: TFT unit and headless WROOM)
 *
 * Features:
 * - NimBLE-Arduino BLE scanning (RuuviTag DF5)
 * - LittleFS for web assets
 * - WiFi + AP fallback + /config form
 * - HTTP server + JSON endpoints + file upload
 * - TFT UI (when HAS_TFT=1)
 * - Automatic timezone detection
 * - Real-time sensor monitoring
 * - Historical data storage
 * - Web-based configuration
 *
 * Hardware Requirements:
 * - ESP32 development board
 * - TFT display (optional, controlled by HAS_TFT)
 * - 4 buttons for navigation (Up, Down, A, B)
 * - WiFi connectivity
 *
 * Software Features:
 * - BLE sensor scanning and data collection
 * - Automatic timezone detection via internet API
 * - Real-time NTP time synchronization
 * - Responsive button handling with debouncing
 * - Card-based UI with navigation
 * - HTTP server for web interface
 * - Historical data storage and retrieval
 * - Memory-efficient binary data compression
 *
 * Configuration:
 * - WiFi credentials stored in /wifi.properties
 * - Sensor names in /names.csv
 * - Web assets served from LittleFS
 *
 * @author pleft
 * @version 0.1
 * @date 2025
 */

#include <Arduino.h>
#include <SPI.h>
#include <FS.h>
#include <LittleFS.h>
#include <NimBLEDevice.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <time.h>
#include <HTTPClient.h>

// Forward declarations
struct WifiCreds;
struct RuuviData;
struct SensorHistory;
struct HistoryPoint;

// ======================= DISPLAY PRESENCE SWITCH ==========================
#ifndef HAS_TFT
#define HAS_TFT 1   // 1 = device with TFT, 0 = headless WROOM (no screen)
#endif

#if HAS_TFT
  #include <TFT_eSPI.h>
#endif
// ==========================================================================

// ============================== USER CONFIG ===============================
/**
 * @brief Hardware configuration for TFT display and buttons
 * 
 * These constants define the GPIO pins and behavior for the TFT display
 * and button interface. Modify these to match your hardware setup.
 */
#if HAS_TFT
// === DISPLAY CONFIGURATION ===
constexpr int      kBacklightPin           = 0;          // Backlight GPIO pin
constexpr bool     kBacklightOnLevel       = LOW;        // Backlight active level (LOW = ON)

// === BUTTON CONFIGURATION ===
// Button pins for BatController configuration
constexpr int      kButtonUpPin            = 26;         // Up button GPIO pin
constexpr int      kButtonDownPin          = 25;         // Down button GPIO pin
constexpr int      kButtonAPin             = 32;         // A button GPIO pin (select)
constexpr int      kButtonBPin             = 35;         // B button GPIO pin (back)
constexpr bool     kButtonActiveLevel      = LOW;        // Button active level (LOW = pressed)

// === UI LAYOUT CONSTANTS ===
constexpr int      kCardHeight             = 40;         // Height of each card in pixels
constexpr int      kCardMargin             = 2;          // Margin between cards in pixels
constexpr int      kHeaderHeight           = 16;         // Header area height in pixels
constexpr int      kMaxVisibleCards        = 2;          // Maximum cards visible on screen
#endif

// === TIMING CONFIGURATION ===
constexpr uint32_t kDrawIntervalMs         = 500;        // Display refresh interval (ms)
constexpr uint32_t kButtonPollIntervalMs  = 10;         // Button polling interval (ms)
constexpr uint32_t kUpdateThrottleMs       = 1000;       // Minimum time between sensor updates (ms)
constexpr uint32_t kStaConnectTimeoutMs    = 12000;      // WiFi connection timeout (ms)

// === NETWORK CONFIGURATION ===
static const char* kApPassword           = "ruuvi1234"; // SoftAP password (minimum 8 characters)

// === NTP AND TIMEZONE CONFIGURATION ===
const char* ntpServer = "pool.ntp.org";                    // NTP server for time synchronization
const char* timezoneApiUrl = "http://worldtimeapi.org/api/ip"; // API for timezone detection
static long gTimezoneOffset = 0;                           // Detected timezone offset in seconds
static bool gTimezoneDetected = false;                     // Whether timezone has been detected
static unsigned long gTimezoneRetryCount = 0;              // Number of timezone detection attempts
constexpr unsigned long kMaxTimezoneRetries = 5;           // Maximum retries before fallback
// ==========================================================================

// --------- Logging ----------
#define LOG(s)     do { Serial.println(s); } while(0)

// --------- TFT (guarded) ----------
#if HAS_TFT
TFT_eSPI    tft;
TFT_eSprite spr = TFT_eSprite(&tft);

// UI State management
enum UIState {
  UI_MAIN_SCREEN,     // Showing sensor cards
  UI_SENSOR_DETAIL,   // Showing full sensor details
  UI_SYSTEM_INFO      // Showing system information
};

static UIState gUIState = UI_MAIN_SCREEN;
static int gSelectedSensorIndex = 0;     // Currently selected sensor (0-based)
static int gScrollOffset = 0;            // Scroll offset for main screen
static int gLastSelectedSensor = -1;     // For change detection
static bool gShowSystemCard = true;      // Whether to show the system info card
static bool gShowDateTimeCard = true;    // Whether to show the time/date card
static int gSystemInfoScroll = 0;        // Scroll offset for system info screen
#endif

// ============================== Timezone Detection =============================
static bool gTimeInitialized = false;    // Whether NTP time has been synchronized

/**
 * @brief Automatically detects timezone using internet API
 * 
 * Uses worldtimeapi.org to detect the local timezone based on IP address.
 * Falls back to UTC+3 after maximum retry attempts.
 * 
 * @note This function is called periodically until timezone is detected
 * @note Requires internet connection to work properly
 */
static void detectTimezone() {
  // Early exit if already detected or no internet
  if (gTimezoneDetected || WiFi.status() != WL_CONNECTED) {
    return;
  }
  
  gTimezoneRetryCount++;
  
  // Fallback to reasonable timezone after max retries
  if (gTimezoneRetryCount > kMaxTimezoneRetries) {
    gTimezoneOffset = 3 * 3600;  // UTC+3 fallback
    gTimezoneDetected = true;
    configTime(gTimezoneOffset, 0, ntpServer);
    LOG("[TIMEZONE] Fallback to UTC+3 after " + String(kMaxTimezoneRetries) + " failed attempts");
    return;
  }
  
  LOG("[TIMEZONE] Detecting timezone (attempt " + String(gTimezoneRetryCount) + "/" + String(kMaxTimezoneRetries) + ")");
  
  HTTPClient http;
  http.begin(timezoneApiUrl);
  http.setTimeout(5000); // 5 second timeout
  
  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    LOG("[TIMEZONE] API response: " + payload);
    
    // Parse JSON response: {"utc_offset":"+03:00",...}
    int offsetStart = payload.indexOf("\"utc_offset\":\"");
    if (offsetStart != -1) {
      offsetStart += 14; // Skip "utc_offset":""
      int offsetEnd = payload.indexOf("\"", offsetStart);
      if (offsetEnd != -1) {
        String offsetStr = payload.substring(offsetStart, offsetEnd);
        LOG("[TIMEZONE] Detected offset: " + offsetStr);
        
        // Parse offset format: "+03:00" or "-05:00"
        if (offsetStr.length() >= 6 && (offsetStr[0] == '+' || offsetStr[0] == '-')) {
          int sign = (offsetStr[0] == '+') ? 1 : -1;
          int hours = offsetStr.substring(1, 3).toInt();
          int minutes = offsetStr.substring(4, 6).toInt();
          
          gTimezoneOffset = sign * (hours * 3600 + minutes * 60);
          gTimezoneDetected = true;
          
          LOG("[TIMEZONE] Set timezone offset: " + String(gTimezoneOffset) + " seconds (" + 
              String(sign * hours) + "h " + String(minutes) + "m)");
          
          // Reconfigure NTP with detected timezone
          configTime(gTimezoneOffset, 0, ntpServer);
          LOG("[TIMEZONE] NTP reconfigured with detected timezone");
        } else {
          LOG("[TIMEZONE] Invalid offset format: " + offsetStr);
        }
      } else {
        LOG("[TIMEZONE] Could not find offset end in response");
      }
    } else {
      LOG("[TIMEZONE] Could not find utc_offset in response");
    }
  } else {
    LOG("[TIMEZONE] API call failed, code: " + String(httpCode));
  }
  
  http.end();
}

// Button state tracking
static bool gButtonUpPressed = false;
static bool gButtonDownPressed = false;
static bool gButtonAPressed = false;
static bool gButtonBPressed = false;
static unsigned long gLastButtonPress = 0;
constexpr unsigned long kButtonDebounceMs = 50;  // Increased debounce for better reliability

// --------- HTTP server & stats ----------
WebServer http(80);
static volatile bool gDataInFlight = false;

static unsigned long gLastDrawMs         = 0;
static unsigned long gLastButtonPollMs  = 0;
static unsigned long gLastHttpActivityMs = 0;
static unsigned long gHttpRequestsTotal  = 0;
static unsigned long gHttpDataRequests   = 0;
static unsigned long gHttpDataBytes      = 0;
static unsigned long gBleAdvertsSeen     = 0;

// --------- Wi-Fi / AP state ----------
static bool       gApMode = false;
static String     gApSsid;
static IPAddress  gApIP;           // typically 192.168.4.1

// ============================= Data Structures ============================

/**
 * @brief Maximum number of BLE devices to track simultaneously
 */
constexpr int kMaxTags  = 4;   // Maximum number of BLE devices to track
constexpr int kMaxNames = 6;   // Maximum number of friendly names to store

/**
 * @brief Structure to store RuuviTag sensor data
 * 
 * Contains all sensor readings from a RuuviTag device including
 * temperature, humidity, pressure, battery, and signal strength.
 */
struct RuuviData {
  String mac;                    // MAC address of the sensor
  float  t = NAN;               // Temperature in Celsius
  float  h = NAN;               // Humidity percentage
  float  p = NAN;               // Pressure in Pascals
  int    rssi = 0;              // Signal strength in dBm
  uint16_t batt_mV = 0;         // Battery voltage in millivolts
  int8_t  txPower  = 0;         // Transmission power level
  uint8_t movement = 0;         // Movement counter
  uint16_t seq     = 0;         // Sequence number
  unsigned long lastSeen = 0;   // Timestamp of last data received
};

/**
 * @brief Structure for friendly name mapping
 * 
 * Maps MAC addresses to human-readable names for display.
 */
struct NameEntry { 
  String mac;   // MAC address
  String name;  // Friendly name
};

/**
 * @brief Structure for WiFi credentials
 * 
 * Stores WiFi network credentials loaded from configuration file.
 */
struct WifiCreds { 
  String ssid;    // Network SSID
  String pass;    // Network password
  bool valid = false;  // Whether credentials are valid
};

RuuviData  gTags[kMaxTags];
int        gTagCount = 0;

NameEntry  gNames[kMaxNames];
int        gNamesCount = 0;

// ============================ ADAPTIVE HISTORY STORAGE ============================
/**
 * @brief Historical data storage configuration
 * 
 * Memory-efficient ring buffer for storing sensor history.
 * Uses binary format with Base64 encoding for web transmission.
 * 
 * Memory usage: 5 sensors × 240 points × 16 bytes = 19,200 bytes total
 * Time coverage: 2 hours at 30-second intervals
 * 
 * IMPROVED: With delta compression, we can store 4x more data efficiently
 * With 5 sensors: 5 sensors × 240 points × 6 bytes = 7,200 bytes (with compression: ~2,880-4,320 bytes)
 */
constexpr int kHistoryMaxSensors = 5;   // Number of sensors to track in history
constexpr int kHistoryPoints     = 240; // Points per sensor (ring buffer size)

// === DELTA COMPRESSION STRUCTURES ===
struct DeltaCompressedPoint {
  int16_t deltaTemp;      // Temperature difference * 100 (2 bytes)
  int16_t deltaHumidity;  // Humidity difference * 100 (2 bytes)  
  int32_t deltaPressure; // Pressure difference in Pa (4 bytes)
  uint16_t timeDelta;     // Time difference in seconds (2 bytes)
} __attribute__((packed));

struct DeltaCompressedSensor {
  char mac[18];           // MAC address (18 bytes)
  uint16_t pointCount;    // Number of points (2 bytes)
  uint32_t baseTimestamp; // Base timestamp for delta calculations (4 bytes)
  float baseTemp;         // Base temperature value (4 bytes)
  float baseHumidity;     // Base humidity value (4 bytes)
  float basePressure;     // Base pressure value (4 bytes)
  // Delta points follow...
} __attribute__((packed));

// Base64 encoding for compressed history data
#include "libb64/cencode.h"
#include "libb64/cdecode.h"

struct HistoryPoint {
  unsigned long timestamp;
  float t, h, p;
  // Removed batt_mV and rssi to save space - only needed for live display
};

struct SensorHistory {
  String mac;
  HistoryPoint points[kHistoryPoints];
  int count = 0;                      // <= kHistoryPoints
  int writeIndex = 0;                 // [0..kHistoryPoints-1]
};

SensorHistory gHistory[kHistoryMaxSensors];
int gHistoryCount = 0;
const unsigned long kHistoryIntervalMs = 30000; // Save every 30 seconds for better resolution
unsigned long gLastHistorySave = 0;

// Binary format for compressed history data
struct BinaryHistoryHeader {
  uint32_t magic;        // 0x48495354 ("HIST")
  uint16_t version;      // Format version
  uint16_t sensorCount;  // Number of sensors
  uint32_t serverTime;   // Server timestamp
} __attribute__((packed));

struct BinarySensorHeader {
  char mac[18];          // MAC address (null-terminated) - increased to 18 to handle full MAC addresses
  uint16_t pointCount;   // Number of data points
} __attribute__((packed));

struct BinaryDataPoint {
  uint32_t timestamp;    // ESP32 millis() timestamp (milliseconds since boot)
  uint16_t temp;         // Temperature * 100 (e.g., 2345 = 23.45°C)
  uint16_t humidity;     // Humidity * 100 (e.g., 6567 = 65.67%)
  uint32_t pressure;     // Pressure in Pa
} __attribute__((packed));

// FreeRTOS spinlock to protect gTags[] / gTagCount
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
portMUX_TYPE gTagsMux = portMUX_INITIALIZER_UNLOCKED;

// CPU usage tracking
static unsigned long gLastCpuCheck = 0;
static float gCpuUsage = 0.0f;

// Calculate CPU usage percentage
static float calculateCpuUsage() {
  // Get task handle for current task (loop task)
  TaskHandle_t currentTask = xTaskGetCurrentTaskHandle();
  
  // Get runtime statistics for all tasks
  UBaseType_t taskCount = uxTaskGetNumberOfTasks();
  if (taskCount == 0) return 0.0f;
  
  // Allocate memory for task status array
  TaskStatus_t* taskStatusArray = (TaskStatus_t*)malloc(taskCount * sizeof(TaskStatus_t));
  if (!taskStatusArray) return 0.0f;
  
  // Get task status information
  UBaseType_t actualTaskCount;
  unsigned long totalRuntime = 0;
  
  // Get runtime statistics
  actualTaskCount = uxTaskGetSystemState(taskStatusArray, taskCount, &totalRuntime);
  
  if (actualTaskCount == 0 || totalRuntime == 0) {
    free(taskStatusArray);
    return 0.0f;
  }
  
  // Find the loop task (usually has highest runtime)
  unsigned long maxRuntime = 0;
  for (UBaseType_t i = 0; i < actualTaskCount; i++) {
    if (taskStatusArray[i].ulRunTimeCounter > maxRuntime) {
      maxRuntime = taskStatusArray[i].ulRunTimeCounter;
    }
  }
  
  // Calculate CPU usage as percentage of total runtime
  float cpuUsage = (maxRuntime * 100.0f) / totalRuntime;
  
  free(taskStatusArray);
  return cpuUsage;
}

// =============================== Utilities ================================
static inline int16_t  be16s(const uint8_t* p) { return (int16_t)((p[0] << 8) | p[1]); }
static inline uint16_t be16 (const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }

// ============================== Button Handling ===============================
#if HAS_TFT
/**
 * @brief Handles button input with debouncing and state management
 * 
 * Processes all four buttons (Up, Down, A, B) with proper debouncing.
 * Manages UI navigation and state transitions.
 * 
 * @note Called every 10ms for maximum responsiveness
 * @note Uses edge detection (press only, not release)
 */
static void handleButtons() {
  unsigned long now = millis();
  
  // Global debounce check - prevents rapid button presses
  if (now - gLastButtonPress < kButtonDebounceMs) {
    return;
  }
  
  // Read all button states in one go for efficiency
  bool upPressed = (digitalRead(kButtonUpPin) == kButtonActiveLevel);
  bool downPressed = (digitalRead(kButtonDownPin) == kButtonActiveLevel);
  bool aPressed = (digitalRead(kButtonAPin) == kButtonActiveLevel);
  bool bPressed = (digitalRead(kButtonBPin) == kButtonActiveLevel);
  
  // === UP BUTTON HANDLING ===
  if (upPressed && !gButtonUpPressed) {
    gLastButtonPress = now;
    gButtonUpPressed = true;
    LOG("[BUTTON] Up pressed");
    
    if (gUIState == UI_MAIN_SCREEN) {
      // Navigate up in main screen
      if (gSelectedSensorIndex > 0) {
        gSelectedSensorIndex--;
        // Adjust scroll if needed
        if (gSelectedSensorIndex < gScrollOffset) {
          gScrollOffset = gSelectedSensorIndex;
        }
        gLastDrawMs = 0; // Force immediate screen update
      }
    } else if (gUIState == UI_SYSTEM_INFO) {
      // Scroll up in system info
      if (gSystemInfoScroll > 0) {
        gSystemInfoScroll--;
        gLastDrawMs = 0;
      }
    }
  } else if (!upPressed) {
    gButtonUpPressed = false;
  }
  
  // === DOWN BUTTON HANDLING ===
  if (downPressed && !gButtonDownPressed) {
    gLastButtonPress = now;
    gButtonDownPressed = true;
    LOG("[BUTTON] Down pressed");
    
    if (gUIState == UI_MAIN_SCREEN) {
      // Get current sensor count safely
      int localCount;
      portENTER_CRITICAL(&gTagsMux);
      localCount = gTagCount;
      portEXIT_CRITICAL(&gTagsMux);
      
      int totalCards = localCount + (gShowSystemCard ? 1 : 0) + (gShowDateTimeCard ? 1 : 0);
      if (gSelectedSensorIndex < totalCards - 1) {
        gSelectedSensorIndex++;
        // Adjust scroll if needed
        if (gSelectedSensorIndex >= gScrollOffset + kMaxVisibleCards) {
          gScrollOffset = gSelectedSensorIndex - kMaxVisibleCards + 1;
        }
        gLastDrawMs = 0; // Force immediate screen update
      }
    } else if (gUIState == UI_SYSTEM_INFO) {
      // Scroll down in system info
      gSystemInfoScroll++;
      gLastDrawMs = 0;
    }
  } else if (!downPressed) {
    gButtonDownPressed = false;
  }
  
  // === A BUTTON HANDLING (SELECT) ===
  if (aPressed && !gButtonAPressed) {
    gLastButtonPress = now;
    gButtonAPressed = true;
    LOG("[BUTTON] A pressed");
    
    if (gUIState == UI_MAIN_SCREEN) {
      // Get current sensor count safely
      int localCount;
      portENTER_CRITICAL(&gTagsMux);
      localCount = gTagCount;
      portEXIT_CRITICAL(&gTagsMux);
      
      int totalCards = localCount + (gShowSystemCard ? 1 : 0) + (gShowDateTimeCard ? 1 : 0);
      
      if (gShowDateTimeCard && gSelectedSensorIndex == 0) {
        // Time/Date card selected
        LOG("[BUTTON] Time/Date card selected");
        gLastDrawMs = 0;
      } else if (gSelectedSensorIndex < localCount + (gShowDateTimeCard ? 1 : 0)) {
        // Regular sensor selected (adjusted for time/date card offset)
        gUIState = UI_SENSOR_DETAIL;
        gLastDrawMs = 0;
      } else if (gShowSystemCard && gSelectedSensorIndex == localCount + (gShowDateTimeCard ? 1 : 0)) {
        // System info card selected
        gUIState = UI_SYSTEM_INFO;
        gSystemInfoScroll = 0; // Reset scroll position
        gLastDrawMs = 0;
      }
    }
  } else if (!aPressed) {
    gButtonAPressed = false;
  }
  
  // === B BUTTON HANDLING (BACK) ===
  if (bPressed && !gButtonBPressed) {
    gLastButtonPress = now;
    gButtonBPressed = true;
    LOG("[BUTTON] B pressed");
    
    // Return to main screen from detail views
    if (gUIState == UI_SENSOR_DETAIL || gUIState == UI_SYSTEM_INFO) {
      gUIState = UI_MAIN_SCREEN;
      gLastDrawMs = 0; // Force immediate screen update
    }
  } else if (!bPressed) {
    gButtonBPressed = false;
  }
}
#endif

static String toUpperMac(const String& mac) { 
  String result = mac; 
  result.toUpperCase(); 
  return result; 
}

static int findTagIdx(const String& mac) {
  for (int i = 0; i < gTagCount; ++i)
    if (gTags[i].mac == mac) return i;
  return -1;
}

// Fits built-in fonts (ASCII only)
static String ellipsize(const String& s, int maxLen) {
  if (maxLen <= 0) return "";
  if ((int)s.length() <= maxLen) return s;
  int keep = maxLen - 3;
  if (keep < 0) keep = 0;
  return s.substring(0, keep) + "...";
}

// ============================== Wi-Fi props ===============================
static WifiCreds loadWifiProps() {
  WifiCreds out;
  if (!LittleFS.exists("/wifi.properties")) {
    LOG("[wifi] /wifi.properties not found");
    return out;
  }
  fs::File f = LittleFS.open("/wifi.properties", "r");
  if (!f) { LOG("[wifi] open failed"); return out; }

  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.isEmpty() || line.startsWith("#")) continue;
    int eq = line.indexOf('=');
    if (eq <= 0) continue;
    String key = line.substring(0, eq); key.trim();
    String val = line.substring(eq + 1); val.trim();
    if (key.equalsIgnoreCase("ssid"))          out.ssid = val;
    else if (key.equalsIgnoreCase("password")) out.pass = val;
  }
  f.close();

  out.valid = (out.ssid.length() && out.pass.length());
  if (!out.valid) LOG("[wifi] invalid /wifi.properties (need ssid= and password=)");
  return out;
}

static bool saveWifiProps(const String& ssid, const String& pass) {
  fs::File w = LittleFS.open("/wifi.properties", "w");
  if (!w) return false;
  w.printf("ssid=%s\npassword=%s\n", ssid.c_str(), pass.c_str());
  w.close();
  return true;
}

// ================= Friendly Names (LittleFS: /names.csv only) =============
static void loadNamesCsv() {
  gNamesCount = 0;
  if (!LittleFS.exists("/names.csv")) { LOG("[names] /names.csv not found"); return; }

  fs::File f = LittleFS.open("/names.csv", "r");
  if (!f) { LOG("[names] open failed"); return; }

  String line;
  while (f.available() && gNamesCount < kMaxNames) {
    line = f.readStringUntil('\n'); line.trim();
    if (line.isEmpty() || line.startsWith("#")) continue;

    int c = line.indexOf(',');
    if (c <= 0) continue;

    String mac = line.substring(0, c); mac.trim();
    String rest = line.substring(c + 1);

    String nm = rest; nm.trim();
    if (mac.length() < 11 || nm.isEmpty()) continue;

    gNames[gNamesCount].mac = toUpperMac(mac);
    gNames[gNamesCount].name = nm;
    gNamesCount++;
  }
  f.close();
}

static String friendlyName(const String& mac) {
  if (mac.length() == 0) return "Unknown";
  String key = toUpperMac(mac);
  for (int i = 0; i < gNamesCount; ++i)
    if (gNames[i].mac == key) return gNames[i].name;
  return key; // fallback to MAC
}

// ============================ Wi-Fi helpers ===============================
static String makeApSsid() {
  uint8_t mac[6]; WiFi.macAddress(mac);
  char ssid[32];
  snprintf(ssid, sizeof(ssid), "RuuviSetup-%02X%02X", mac[4], mac[5]);
  return String(ssid);
}

static void ensureAPStarted() {
  if (gApMode) return;
  gApSsid = makeApSsid();

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(100);
  WiFi.mode(WIFI_AP);

  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  bool ok = WiFi.softAP(gApSsid.c_str(), kApPassword, 1, 0, 4);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);

  gApIP   = WiFi.softAPIP();
  gApMode = ok;
}

static bool tryConnectSTA(const WifiCreds& wc, uint32_t timeoutMs) {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(wc.ssid.c_str(), wc.pass.c_str());

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) {
    delay(300); Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WiFi] Connected, IP: %s RSSI:%d dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
    if (MDNS.begin("home")) Serial.println("[mDNS] http://home.local/");
    return true;
  }
  Serial.println("[WiFi] Connect timeout");
  return false;
}

// Event hook: auto-start AP after repeated failures during runtime
void onWiFiEvent(arduino_event_id_t event) {
  static uint8_t failCount = 0;
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      failCount = 0;
      Serial.println("[WiFi] STA connected");
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.println("[WiFi] STA disconnected");
      if (!gApMode && ++failCount >= 4) { ensureAPStarted(); }
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.printf("[WiFi] IP: %s\n", WiFi.localIP().toString().c_str());
      break;
    case ARDUINO_EVENT_WIFI_STA_LOST_IP:
      Serial.println("[WiFi] Lost IP");
      break;
    default: break;
  }
}

// ============================== Ruuvi DF5 ================================
static bool decodeRuuviDF5(const std::string& mfg, int rssi, String macStr) {
  const uint8_t* d = reinterpret_cast<const uint8_t*>(mfg.data());
  const size_t   n = mfg.size();
  if (n < 26 || d[0] != 0x99 || d[1] != 0x04 || d[2] != 0x05) return false;

  const uint8_t* p = d + 3;

  const float temperature = be16s(p + 0) / 200.0f;
  const float humidity    = be16 (p + 2) * 0.0025f;
  const float pressurePa  = 50000.0f + be16(p + 4);

  const uint16_t powerField = be16(p + 12);
  const uint16_t batt_mV    = (powerField >> 5) + 1600;
  const int8_t   txPower    = ((powerField & 0x1F) * 2) - 40;
  const uint8_t  movement   = p[14];
  const uint16_t seq        = be16(p + 15);

  const unsigned long now = millis();

  // Safer critical section - minimize time in critical section
  portENTER_CRITICAL(&gTagsMux);
  int idx = findTagIdx(macStr);
  if (idx == -1 && gTagCount < kMaxTags) {
    idx = gTagCount++;
    gTags[idx].mac = macStr;
    gTags[idx].lastSeen = 0;
  }
  portEXIT_CRITICAL(&gTagsMux);

  // Update data outside critical section to reduce lock time
  if (idx >= 0) {
    portENTER_CRITICAL(&gTagsMux);
    if (idx < gTagCount && now - gTags[idx].lastSeen >= kUpdateThrottleMs) {
      gTags[idx].t        = temperature;
      gTags[idx].h        = humidity;
      gTags[idx].p        = pressurePa;
      gTags[idx].rssi     = rssi;
      gTags[idx].batt_mV  = batt_mV;
      gTags[idx].txPower  = txPower;
      gTags[idx].movement = movement;
      gTags[idx].seq      = seq;
      gTags[idx].lastSeen = now;
    }
    portEXIT_CRITICAL(&gTagsMux);
  }
  return true;
}

// ============================ ADAPTIVE HISTORY STORAGE ====================
static void saveHistoryData() {
  unsigned long now = millis();
  if (now - gLastHistorySave < kHistoryIntervalMs) return;
  gLastHistorySave = now;

  // Safer critical section access with timeout protection
  portENTER_CRITICAL(&gTagsMux);
  int localCount = gTagCount;
  portEXIT_CRITICAL(&gTagsMux);

  for (int i = 0; i < localCount; ++i) {
    RuuviData tag;
    portENTER_CRITICAL(&gTagsMux);
    if (i < gTagCount) tag = gTags[i];
    portEXIT_CRITICAL(&gTagsMux);

    // Find or create history entry for this MAC
    int histIdx = -1;
    for (int j = 0; j < gHistoryCount; ++j) {
      if (gHistory[j].mac == tag.mac) { histIdx = j; break; }
    }

    if (histIdx == -1 && gHistoryCount < kHistoryMaxSensors) {
      histIdx = gHistoryCount++;
      gHistory[histIdx].mac = tag.mac;
      gHistory[histIdx].count = 0;
      gHistory[histIdx].writeIndex = 0;
      LOG("[History] Added sensor " + String(gHistoryCount) + "/" + String(kHistoryMaxSensors) + ": " + tag.mac);
    }

    if (histIdx >= 0) {
      SensorHistory& hist = gHistory[histIdx];

      // Simple circular buffer - just add new data point
      HistoryPoint& point = hist.points[hist.writeIndex];
      point.timestamp = now;
      point.t = tag.t;
      point.h = tag.h;
      point.p = tag.p;
      // Note: battery and RSSI not stored in history to save space

      hist.writeIndex = (hist.writeIndex + 1) % kHistoryPoints;
      if (hist.count < kHistoryPoints) hist.count++;
      
      // Debug: Show history status for this sensor (rotate through all sensors to avoid spam)
      static unsigned long lastHistoryLog = 0;
      static int lastLoggedSensor = -1;
      if (millis() - lastHistoryLog > 30000) { // Log every 30 seconds
        lastHistoryLog = millis();
        lastLoggedSensor = (lastLoggedSensor + 1) % gHistoryCount; // Rotate through sensors
        if (lastLoggedSensor == histIdx) { // Only log for the current sensor in rotation
          LOG("[History] Saved data for " + tag.mac + " (points: " + String(hist.count) + "/" + String(kHistoryPoints) + ")");
        }
      }
    }
  }
}

// ============================ NimBLE (2.x) ===============================
class RuuviScanCB : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* adv) override {
    gBleAdvertsSeen++;
    if (!adv->haveManufacturerData()) return;

    try {
      std::string mfg = adv->getManufacturerData();
      if (mfg.size() < 2) return;

      const uint8_t* d = reinterpret_cast<const uint8_t*>(mfg.data());
      std::string addrStd = adv->getAddress().toString();
      String mac(addrStd.c_str());

      if (d[0] == 0x99 && d[1] == 0x04) {
        decodeRuuviDF5(mfg, adv->getRSSI(), mac);
      }
    } catch (...) {
      // Catch any exceptions in BLE callback to prevent crashes
      LOG("[BLE] Exception in onResult callback");
    }
  }
};

// ============================== UI / Drawing =============================
#if HAS_TFT
static void drawSensorCard(int cardIndex, int y, bool isSelected, const RuuviData& sensor) {
  const int cardWidth = spr.width() - 4;  // Full width minus margins
  const int x = 2;
  
  // Card background
  uint16_t bgColor = isSelected ? TFT_DARKGREY : TFT_BLACK;
  uint16_t borderColor = isSelected ? TFT_WHITE : TFT_DARKGREY;
  
  spr.fillRect(x, y, cardWidth, kCardHeight, bgColor);
  spr.drawRect(x, y, cardWidth, kCardHeight, borderColor);
  
  // FINAL FIX - Use TL_DATUM for both sides with calculated positions
  const int margin = 4;
  const int leftX = x + margin + 3;                // Left side: 9px (moved 3px right for breathing room)
  const int rightStartX = x + cardWidth - 60;     // Right side: 96px (156-60)
  
  // Row 1: Sensor name (left) and Temperature (right)
  String name = ellipsize(friendlyName(sensor.mac), 12); // Max 12 chars = 72px
  spr.setTextFont(1);
  spr.setTextColor(TFT_WHITE, bgColor);
  spr.setTextDatum(TL_DATUM);
  spr.setCursor(leftX, y + 8);
  spr.print(name);
  
  // Temperature on first row (right side) - positioned at calculated X
  if (!isnan(sensor.t)) {
    String tempStr = String(sensor.t, 1) + "C";  // Removed degree symbol for better display
    spr.setTextFont(1);
    spr.setTextColor(TFT_RED, bgColor);
    spr.setTextDatum(TL_DATUM);
    spr.setCursor(rightStartX, y + 8);
    spr.print(tempStr);
  }
  
  // Row 2: Humidity (left) and Age (right)
  if (!isnan(sensor.h)) {
    spr.setTextFont(1);
    spr.setTextColor(TFT_CYAN, bgColor);
    spr.setTextDatum(TL_DATUM);
    spr.setCursor(leftX, y + 26);
    spr.print("H:" + String(sensor.h, 0) + "%");
  }
  
  // Age indicator (right side) - positioned at calculated X
  unsigned long age = (millis() - sensor.lastSeen) / 1000;
  String ageStr;
  if (age < 60) {
    ageStr = String(age) + "s ago";
  } else if (age < 3600) {
    ageStr = String(age / 60) + "m ago";
  } else {
    ageStr = String(age / 3600) + "h ago";
  }
  
  spr.setTextFont(1);
  spr.setTextColor(TFT_GREEN, bgColor);
  spr.setTextDatum(TL_DATUM);
  spr.setCursor(rightStartX, y + 26);
  spr.print(ageStr);
  
  // Selection indicator (left side) - back to original position
  if (isSelected) {
    spr.fillRect(x + 1, y + 1, 3, kCardHeight - 2, TFT_YELLOW);
  }
}

static void drawDateTimeCard(int y, bool isSelected) {
  const int cardWidth = spr.width() - 4;  // Full width minus margins
  const int x = 2;
  
  // Card background
  uint16_t bgColor = isSelected ? TFT_DARKGREY : TFT_BLACK;
  uint16_t borderColor = isSelected ? TFT_WHITE : TFT_DARKGREY;
  
  spr.fillRect(x, y, cardWidth, kCardHeight, bgColor);
  spr.drawRect(x, y, cardWidth, kCardHeight, borderColor);
  
  // Position calculations
  const int margin = 4;
  const int leftX = x + margin + 3;
  const int rightStartX = x + cardWidth - 60;
  
  // Get real time from NTP
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    // Format time
    char timeStr[10];
    strftime(timeStr, sizeof(timeStr), "%H:%M", &timeinfo);
    
    // Format date with year
    char dateStr[16];
    strftime(dateStr, sizeof(dateStr), "%d/%m/%y", &timeinfo);
    
    // Row 1: "Time/Date" label (left) and Time (right)
    spr.setTextFont(1);
    spr.setTextColor(TFT_WHITE, bgColor);
    spr.setTextDatum(TL_DATUM);
    spr.setCursor(leftX, y + 8);
    spr.print("Time/Date");
    
    // Time on first row (right side)
    spr.setTextFont(1);
    spr.setTextColor(TFT_GREEN, bgColor);
    spr.setTextDatum(TL_DATUM);
    spr.setCursor(rightStartX, y + 8);
    spr.print(timeStr);
    
    // Row 2: Date (left) and simple graphics (right)
    spr.setTextFont(1);
    spr.setTextColor(TFT_CYAN, bgColor);
    spr.setTextDatum(TL_DATUM);
    spr.setCursor(leftX, y + 26);
    spr.print(dateStr);
    
    // Simple graphics based on time of day (use simple ASCII)
    String timeIcon = "";
    if (timeinfo.tm_hour >= 6 && timeinfo.tm_hour < 18) {
      timeIcon = "*";  // Sun for daytime (6 AM - 6 PM)
    } else {
      timeIcon = "o";  // Moon for nighttime
    }
    
    // Debug logging for timezone issues
    static unsigned long lastDebugTime = 0;
    if (millis() - lastDebugTime > 10000) { // Log every 10 seconds
      lastDebugTime = millis();
      LOG("[TIMEZONE] Current time: " + String(timeinfo.tm_hour) + ":" + 
          String(timeinfo.tm_min) + ", Timezone detected: " + String(gTimezoneDetected) + 
          ", Offset: " + String(gTimezoneOffset));
    }
    
    spr.setTextFont(1);
    spr.setTextColor(TFT_YELLOW, bgColor);
    spr.setTextDatum(TL_DATUM);
    spr.setCursor(rightStartX, y + 26);
    spr.print(timeIcon);
    
  } else {
    // Check connection status and show appropriate message
    spr.setTextFont(1);
    spr.setTextColor(TFT_WHITE, bgColor);
    spr.setTextDatum(TL_DATUM);
    spr.setCursor(leftX, y + 8);
    spr.print("Time/Date");
    
    if (WiFi.status() != WL_CONNECTED) {
      // No internet connection
      spr.setTextFont(1);
      spr.setTextColor(TFT_RED, bgColor);
      spr.setTextDatum(TL_DATUM);
      spr.setCursor(leftX, y + 26);
      spr.print("Internet required");
    } else if (!gTimezoneDetected) {
      // Internet connected but timezone not detected yet
      spr.setTextFont(1);
      spr.setTextColor(TFT_YELLOW, bgColor);
      spr.setTextDatum(TL_DATUM);
      spr.setCursor(leftX, y + 26);
      spr.print("Detecting timezone...");
    } else {
      // Internet connected, timezone detected, but time sync failed
      spr.setTextFont(1);
      spr.setTextColor(TFT_ORANGE, bgColor);
      spr.setTextDatum(TL_DATUM);
      spr.setCursor(leftX, y + 26);
      spr.print("Time sync failed");
    }
  }
  
  // Selection indicator
  if (isSelected) {
    spr.fillRect(x + 1, y + 1, 3, kCardHeight - 2, TFT_YELLOW);
  }
}

static void drawSystemCard(int y, bool isSelected) {
  const int cardWidth = spr.width() - 4;  // Full width minus margins
  const int x = 2;
  
  // Card background
  uint16_t bgColor = isSelected ? TFT_DARKGREY : TFT_BLACK;
  uint16_t borderColor = isSelected ? TFT_WHITE : TFT_DARKGREY;
  
  spr.fillRect(x, y, cardWidth, kCardHeight, bgColor);
  spr.drawRect(x, y, cardWidth, kCardHeight, borderColor);
  
  // FINAL FIX - Use TL_DATUM for both sides with calculated positions
  const int margin = 4;
  const int leftX = x + margin + 3;                // Left side: 9px (moved 3px right for breathing room)
  const int rightStartX = x + cardWidth - 60;     // Right side: 96px (156-60)
  
  // Row 1: "System Info" label (left) and Free heap (right)
  spr.setTextFont(1);
  spr.setTextColor(TFT_YELLOW, bgColor);
  spr.setTextDatum(TL_DATUM);
  spr.setCursor(leftX, y + 8);
  spr.print("System Info");
  
  // Free heap info on first row (right side) - positioned at calculated X
  String heapStr = String(ESP.getFreeHeap() / 1024) + " kB";
  spr.setTextFont(1);
  spr.setTextColor(TFT_GREEN, bgColor);
  spr.setTextDatum(TL_DATUM);
  spr.setCursor(rightStartX, y + 8);
  spr.print(heapStr);
  
  // Row 2: WiFi status (left) and uptime (right)
  String wifiStatus;
  uint16_t wifiColor;
  if (WiFi.status() == WL_CONNECTED) {
    wifiStatus = "WiFi:OK";
    wifiColor = TFT_GREEN;
  } else if (gApMode) {
    wifiStatus = "WiFi:AP";
    wifiColor = TFT_YELLOW;
  } else {
    wifiStatus = "WiFi:--";
    wifiColor = TFT_RED;
  }
  
  spr.setTextFont(1);
  spr.setTextColor(wifiColor, bgColor);
  spr.setTextDatum(TL_DATUM);
  spr.setCursor(leftX, y + 26);
  spr.print(wifiStatus);
  
  // Uptime on second row (right side) - positioned at calculated X
  unsigned long uptime = millis() / 1000;
  String uptimeStr;
  if (uptime < 3600) {
    uptimeStr = String(uptime / 60) + "m ago";
  } else {
    uptimeStr = String(uptime / 3600) + "h ago";
  }
  
  spr.setTextFont(1);
  spr.setTextColor(TFT_CYAN, bgColor);
  spr.setTextDatum(TL_DATUM);
  spr.setCursor(rightStartX, y + 26);
  spr.print(uptimeStr);
  
  // Selection indicator (left side) - back to original position
  if (isSelected) {
    spr.fillRect(x + 1, y + 1, 3, kCardHeight - 2, TFT_YELLOW);
  }
}

static void drawMainScreen() {
  spr.fillSprite(TFT_BLACK);
  
  // Header
  spr.setTextFont(2);
  spr.setTextColor(TFT_CYAN, TFT_BLACK);
  spr.setTextDatum(TC_DATUM);
  spr.drawString("Home-Dashboard", spr.width() / 2, 2);
  
  int localCount;
  portENTER_CRITICAL(&gTagsMux);
  localCount = gTagCount;
  portEXIT_CRITICAL(&gTagsMux);
  
  // Adjust scroll bounds
  int totalCards = localCount + (gShowSystemCard ? 1 : 0) + (gShowDateTimeCard ? 1 : 0);
  if (gSelectedSensorIndex >= totalCards && totalCards > 0) {
    gSelectedSensorIndex = totalCards - 1;
  }
  if (gSelectedSensorIndex < 0) {
    gSelectedSensorIndex = 0;
  }
  if (gScrollOffset >= totalCards) {
    gScrollOffset = max(0, totalCards - kMaxVisibleCards);
  }
  
  // Draw sensor cards
  if (totalCards == 0 || (localCount == 0 && !gShowSystemCard && !gShowDateTimeCard)) {
    spr.setTextFont(1);
    spr.setTextColor(TFT_WHITE, TFT_BLACK);
    spr.setTextDatum(TC_DATUM);
    spr.drawString("Scanning...", spr.width() / 2, spr.height() / 2);
  } else {
    int cardsToShow = min(kMaxVisibleCards, totalCards - gScrollOffset);
    for (int i = 0; i < cardsToShow; ++i) {
      int cardIndex = gScrollOffset + i;
      int y = kHeaderHeight + i * (kCardHeight + kCardMargin);
      bool isSelected = (cardIndex == gSelectedSensorIndex);
      
      if (gShowDateTimeCard && cardIndex == 0) {
        // Time/Date card (always first)
        drawDateTimeCard(y, isSelected);
      } else if (cardIndex < localCount + (gShowDateTimeCard ? 1 : 0)) {
        // Regular sensor card (adjusted for time/date card offset)
        int sensorIndex = cardIndex - (gShowDateTimeCard ? 1 : 0);
        RuuviData sensor;
        portENTER_CRITICAL(&gTagsMux);
        if (sensorIndex < gTagCount) sensor = gTags[sensorIndex];
        portEXIT_CRITICAL(&gTagsMux);
        
        drawSensorCard(sensorIndex, y, isSelected, sensor);
      } else if (gShowSystemCard && cardIndex == localCount + (gShowDateTimeCard ? 1 : 0)) {
        // System info card
        drawSystemCard(y, isSelected);
      }
    }
  }
  
  // Footer info
  spr.setTextFont(1);
  spr.setTextColor(TFT_DARKGREY, TFT_BLACK);
  spr.setTextDatum(TL_DATUM);
  spr.setCursor(2, spr.height() - 8);
  spr.printf("%d/%d", gSelectedSensorIndex + 1, totalCards);
  
  spr.setTextDatum(TR_DATUM);
  spr.setCursor(spr.width() - 2, spr.height() - 8);
  spr.printf("h:%ukB", ESP.getFreeHeap() / 1024);
  
  spr.pushSprite(0, 0);
}

static void drawSensorDetail() {
  spr.fillSprite(TFT_BLACK);
  
  // Calculate actual sensor index accounting for time/date card offset
  int actualSensorIndex = gSelectedSensorIndex - (gShowDateTimeCard ? 1 : 0);
  
  if (actualSensorIndex < 0 || actualSensorIndex >= gTagCount) {
    spr.setTextFont(1);
    spr.setTextColor(TFT_WHITE, TFT_BLACK);
    spr.setTextDatum(TC_DATUM);
    spr.drawString("No sensor selected", spr.width() / 2, spr.height() / 2);
    spr.pushSprite(0, 0);
    return;
  }
  
  RuuviData sensor;
  portENTER_CRITICAL(&gTagsMux);
  if (actualSensorIndex < gTagCount) sensor = gTags[actualSensorIndex];
  portEXIT_CRITICAL(&gTagsMux);
  
  String name = friendlyName(sensor.mac);
  
  // Title
  spr.setTextFont(1);
  spr.setTextColor(TFT_CYAN, TFT_BLACK);
  spr.setTextDatum(TC_DATUM);
  spr.drawString(name, spr.width() / 2, 5);
  
  int y = 20;
  const int lineHeight = 14;
  
  // Temperature
  if (!isnan(sensor.t)) {
    spr.setTextFont(2);
    spr.setTextColor(TFT_RED, TFT_BLACK);
    spr.setTextDatum(TL_DATUM);
    spr.setCursor(5, y);
    spr.print("Temp: " + String(sensor.t, 2) + "°C");
    y += lineHeight + 5;
  }
  
  // Humidity
  if (!isnan(sensor.h)) {
    spr.setTextFont(2);
    spr.setTextColor(TFT_CYAN, TFT_BLACK);
    spr.setTextDatum(TL_DATUM);
    spr.setCursor(5, y);
    spr.print("Humidity: " + String(sensor.h, 1) + "%");
    y += lineHeight + 5;
  }
  
  // Pressure
  if (!isnan(sensor.p)) {
    spr.setTextFont(1);
    spr.setTextColor(TFT_WHITE, TFT_BLACK);
    spr.setTextDatum(TL_DATUM);
    spr.setCursor(5, y);
    spr.print("Pressure: " + String(sensor.p / 100.0, 1) + " hPa");
    y += lineHeight;
  }
  
  // Battery
  if (sensor.batt_mV > 0) {
    spr.setTextFont(1);
    spr.setTextColor(TFT_GREEN, TFT_BLACK);
    spr.setTextDatum(TL_DATUM);
    spr.setCursor(5, y);
    spr.print("Battery: " + String(sensor.batt_mV) + " mV");
    y += lineHeight;
  }
  
  // RSSI
  spr.setTextFont(1);
  spr.setTextColor(TFT_YELLOW, TFT_BLACK);
  spr.setTextDatum(TL_DATUM);
  spr.setCursor(5, y);
  spr.print("RSSI: " + String(sensor.rssi) + " dBm");
  y += lineHeight;
  
  // Last seen
  unsigned long age = (millis() - sensor.lastSeen) / 1000;
  spr.setTextFont(1);
  spr.setTextColor(TFT_GREEN, TFT_BLACK);
  spr.setTextDatum(TL_DATUM);
  spr.setCursor(5, y);
  if (age < 60) {
    spr.print("Last seen: " + String(age) + "s ago");
  } else {
    spr.print("Last seen: " + String(age / 60) + "m ago");
  }
  
  // Back instruction
  spr.setTextFont(1);
  spr.setTextColor(TFT_DARKGREY, TFT_BLACK);
  spr.setTextDatum(TC_DATUM);
  spr.drawString("B=Back", spr.width() / 2, spr.height() - 8);
  
  spr.pushSprite(0, 0);
}

static void drawSystemInfo() {
  spr.fillSprite(TFT_BLACK);
  
  // Title
  spr.setTextFont(1);
  spr.setTextColor(TFT_CYAN, TFT_BLACK);
  spr.setTextDatum(TC_DATUM);
  spr.drawString("System Info", spr.width() / 2, 5);
  
  // Calculate scroll area (leave space for title and back instruction)
  const int startY = 20;
  const int endY = spr.height() - 15;
  const int visibleHeight = endY - startY;
  const int lineHeight = 11;
  const int maxVisibleLines = visibleHeight / lineHeight;
  
  // System info items array
  String infoLines[10];
  int lineCount = 0;
  
  // CPU usage (moved to top)
  infoLines[lineCount++] = "CPU Usage: " + String(gCpuUsage, 1) + "%";
  
  // Free heap
  infoLines[lineCount++] = "Free Heap: " + String(ESP.getFreeHeap() / 1024) + " kB";
  
  // Min free heap
  infoLines[lineCount++] = "Min Heap: " + String(ESP.getMinFreeHeap() / 1024) + " kB";
  
  // Uptime
  unsigned long uptime = millis() / 1000;
  if (uptime < 3600) {
    infoLines[lineCount++] = "Uptime: " + String(uptime / 60) + " min";
  } else {
    infoLines[lineCount++] = "Uptime: " + String(uptime / 3600) + "h " + String((uptime % 3600) / 60) + "m";
  }
  
  // Sensor count
  int localCount;
  portENTER_CRITICAL(&gTagsMux);
  localCount = gTagCount;
  portEXIT_CRITICAL(&gTagsMux);
  infoLines[lineCount++] = "Sensors: " + String(localCount);
  
  // HTTP requests
  infoLines[lineCount++] = "HTTP Reqs: " + String(gHttpRequestsTotal);
  
  // BLE advertisements seen
  infoLines[lineCount++] = "BLE Adverts: " + String(gBleAdvertsSeen);
  
  // WiFi status
  if (WiFi.status() == WL_CONNECTED) {
    infoLines[lineCount++] = "WiFi: Connected";
    infoLines[lineCount++] = "IP: " + WiFi.localIP().toString();
  } else if (gApMode) {
    infoLines[lineCount++] = "WiFi: AP Mode";
    infoLines[lineCount++] = "SSID: " + gApSsid;
  } else {
    infoLines[lineCount++] = "WiFi: Disconnected";
  }
  
  // Limit scroll range
  int maxScroll = max(0, lineCount - maxVisibleLines);
  if (gSystemInfoScroll > maxScroll) {
    gSystemInfoScroll = maxScroll;
  }
  if (gSystemInfoScroll < 0) {
    gSystemInfoScroll = 0;
  }
  
  // Draw visible lines
  spr.setTextFont(1);
  spr.setTextColor(TFT_WHITE, TFT_BLACK);
  spr.setTextDatum(TL_DATUM);
  
  for (int i = 0; i < maxVisibleLines && (i + gSystemInfoScroll) < lineCount; i++) {
    int lineIndex = i + gSystemInfoScroll;
    int y = startY + i * lineHeight;
    
    // Color coding for different types of info
    if (infoLines[lineIndex].startsWith("WiFi: Connected")) {
      spr.setTextColor(TFT_GREEN, TFT_BLACK);
    } else if (infoLines[lineIndex].startsWith("WiFi: AP Mode")) {
      spr.setTextColor(TFT_YELLOW, TFT_BLACK);
    } else if (infoLines[lineIndex].startsWith("WiFi: Disconnected")) {
      spr.setTextColor(TFT_RED, TFT_BLACK);
    } else {
      spr.setTextColor(TFT_WHITE, TFT_BLACK);
    }
    
    spr.setCursor(5, y);
    spr.print(infoLines[lineIndex]);
  }
  
  // Scroll indicator - same Y as B=Back
  if (maxScroll > 0) {
    spr.setTextFont(1);
    spr.setTextColor(TFT_DARKGREY, TFT_BLACK);
    spr.setTextDatum(TL_DATUM);
    spr.setCursor(5, spr.height() - 8);
    spr.printf("%d/%d", gSystemInfoScroll + 1, maxScroll + 1);
  }
  
  // Back instruction
  spr.setTextFont(1);
  spr.setTextColor(TFT_DARKGREY, TFT_BLACK);
  spr.setTextDatum(TC_DATUM);
  spr.drawString("B=Back", spr.width() / 2, spr.height() - 8);
  
  spr.pushSprite(0, 0);
}

static void drawDashboard() {
  // Draw appropriate screen based on UI state
  if (gUIState == UI_MAIN_SCREEN) {
    drawMainScreen();
  } else if (gUIState == UI_SENSOR_DETAIL) {
    drawSensorDetail();
  } else if (gUIState == UI_SYSTEM_INFO) {
    drawSystemInfo();
  }
}
#else
// Headless build: no screen drawing
static inline void drawDashboard() {}
#endif

// ============================== HTTP server ==============================
static String contentTypeFor(const String& path) {
  if      (path.endsWith(".html")) return "text/html";
  else if (path.endsWith(".css"))  return "text/css";
  else if (path.endsWith(".js"))   return "application/javascript";
  else if (path.endsWith(".json")) return "application/json";
  else if (path.endsWith(".png"))  return "image/png";
  else if (path.endsWith(".jpg") || path.endsWith(".jpeg")) return "image/jpeg";
  else if (path.endsWith(".svg"))  return "image/svg+xml";
  return "text/plain";
}

static bool serveFile(const String& path) {
  if (!LittleFS.exists(path)) { LOG("[HTTP] 404 (missing)"); return false; }
  fs::File f = LittleFS.open(path, "r");
  if (!f) { LOG("[HTTP] 404 (open failed)"); return false; }
  size_t sent = http.streamFile(f, contentTypeFor(path));
  (void)sent;
  f.close();
  return true;
}

// Root -> /index.html (if present)
static void handleRoot() {
  gHttpRequestsTotal++; gLastHttpActivityMs = millis();
  LOG("[HTTP] GET / - Free heap: " + String(ESP.getFreeHeap()));
  http.sendHeader("Cache-Control","no-cache, no-store, must-revalidate");
  if (!serveFile("/index.html")) http.send(200, "text/plain",
    "Upload /index.html or use /data JSON.\nTry /upload or /config.");
}

static void handleStatic() {
  gHttpRequestsTotal++; gLastHttpActivityMs = millis();
  if (!serveFile(http.uri())) http.send(404, "text/plain", "Not found");
}

// --- /data (chunked JSON)
static inline void sendChunk(const char* s) {
  if (s && *s) { gHttpDataBytes += strlen(s); http.sendContent(s); }
}

static const char* fmtFloatOrNull(char* tmp, size_t tmpsz, float v, uint8_t prec) {
  if (isnan(v)) return "null";
  dtostrf(v, 0, prec, tmp);
  return tmp;
}

static String fmtFloatOrNullString(float v, uint8_t prec) {
  if (isnan(v)) return "null";
  return String(v, (unsigned int)prec);
}

static void handleDataStream() {
  if (gDataInFlight) { 
    LOG("[HTTP] GET /data - BUSY (request blocked)");
    http.send(503, "application/json", "{\"error\":\"busy\"}"); 
    return; 
  }
  gDataInFlight = true;
  
  // Add timeout protection - if this function takes too long, force reset
  unsigned long startTime = millis();

  gHttpRequestsTotal++; gHttpDataRequests++; gLastHttpActivityMs = millis();
  gHttpDataBytes = 0;
  LOG("[HTTP] GET /data - START - Free heap: " + String(ESP.getFreeHeap()));

  http.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  http.sendHeader("Connection", "close");
  http.setContentLength(CONTENT_LENGTH_UNKNOWN);
  http.send(200, "application/json", "");   // headers

  char buf[192];
  char fbuf[24];
  sendChunk("{\"tags\":[");
  unsigned long now = millis();

  int localCount;
  portENTER_CRITICAL(&gTagsMux);
  localCount = gTagCount;
  portEXIT_CRITICAL(&gTagsMux);

  for (int i = 0; i < localCount; ++i) {
    // Timeout check - if this is taking too long, abort
    if (millis() - startTime > 5000) {  // 5 second timeout
      LOG("[ERROR] handleDataStream timeout after 5s, aborting");
      gDataInFlight = false;
      http.send(500, "application/json", "{\"error\":\"timeout\"}");
      return;
    }
    
    RuuviData t;
    portENTER_CRITICAL(&gTagsMux);
    if (i < gTagCount) t = gTags[i];
    portEXIT_CRITICAL(&gTagsMux);

    // Use friendly name for better user experience
    String friendly = friendlyName(t.mac);
    snprintf(buf, sizeof(buf),
         "%s{\"mac\":\"%s\",\"name\":\"%s\",",
         (i ? "," : ""),
         t.mac.c_str(),
         friendly.c_str());
    sendChunk(buf);

    const char* tstr = fmtFloatOrNull(fbuf, sizeof(fbuf), t.t, 2);
    snprintf(buf, sizeof(buf), "\"t\":%s,", tstr); sendChunk(buf);

    const char* hstr = fmtFloatOrNull(fbuf, sizeof(fbuf), t.h, 2);
    snprintf(buf, sizeof(buf), "\"h\":%s,", hstr); sendChunk(buf);

    if (isnan(t.p)) sendChunk("\"p\":null,");
    else { snprintf(buf, sizeof(buf), "\"p\":%lu,", (unsigned long)t.p); sendChunk(buf); }

    if (t.batt_mV == 0) sendChunk("\"batt\":null,");
    else { snprintf(buf, sizeof(buf), "\"batt\":%u,", (unsigned)t.batt_mV); sendChunk(buf); }

    snprintf(buf, sizeof(buf), "\"rssi\":%d,", t.rssi); sendChunk(buf);

    snprintf(buf, sizeof(buf), "\"age\":%lu}", (unsigned long)((now - t.lastSeen) / 1000)); sendChunk(buf);

    delay(1); // yield
  }

  sendChunk("]}");
  http.sendContent(""); // end chunked
  gDataInFlight = false;
  LOG("[HTTP] GET /data - END - Bytes sent: " + String(gHttpDataBytes) + ", Free heap: " + String(ESP.getFreeHeap()));
}

// --- /data_plain (non-chunked)
static void handleDataPlain() {
  gHttpRequestsTotal++; gHttpDataRequests++; gLastHttpActivityMs = millis();

  String out;
  out.reserve(512 + gTagCount * 160);
  out += F("{\"tags\":[");
  unsigned long now = millis();

  int localCount;
  portENTER_CRITICAL(&gTagsMux);
  localCount = gTagCount;
  portEXIT_CRITICAL(&gTagsMux);

  for (int i = 0; i < localCount; ++i) {
    RuuviData t;
    portENTER_CRITICAL(&gTagsMux);
    if (i < gTagCount) t = gTags[i];
    portEXIT_CRITICAL(&gTagsMux);

    if (i) out += ',';
    out += F("{\"mac\":\""); out += t.mac; out += '"';
    out += F(",\"name\":\""); out += friendlyName(t.mac); out += '"';

    out += F(",\"t\":"); out += (isnan(t.t) ? F("null") : String(t.t, 2));
    out += F(",\"h\":"); out += (isnan(t.h) ? F("null") : String(t.h, 2));
    out += F(",\"p\":"); out += (isnan(t.p) ? F("null") : String((uint32_t)t.p));
    out += F(",\"batt\":"); out += String((unsigned)t.batt_mV);
    out += F(",\"rssi\":"); out += String(t.rssi);

    out += F(",\"age\":");  out += String((now - t.lastSeen) / 1000);
    out += '}';
  }
  out += F("]}");

  http.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  http.sendHeader("Connection", "close");
  http.send(200, "application/json", out);
}

// === DELTA COMPRESSION FUNCTIONS ===

/**
 * @brief Compress sensor data using delta compression
 * @param input Raw sensor data
 * @param output Compressed data buffer
 * @param maxOutputSize Maximum output buffer size
 * @return Actual compressed size, or 0 on error
 */
static size_t compressDelta(const SensorHistory* input, uint8_t* output, size_t maxOutputSize) {
  if (!input || !output || maxOutputSize < sizeof(DeltaCompressedSensor)) {
    LOG("[COMPRESSION] compressDelta failed: input=" + String(input ? "OK" : "NULL") + 
        ", output=" + String(output ? "OK" : "NULL") + 
        ", maxSize=" + String(maxOutputSize) + 
        ", minSize=" + String(sizeof(DeltaCompressedSensor)));
    return 0;
  }
  
  size_t offset = 0;
  
  // Write sensor header
  DeltaCompressedSensor* header = (DeltaCompressedSensor*)(output + offset);
  strncpy(header->mac, input->mac.c_str(), sizeof(header->mac) - 1);
  header->mac[sizeof(header->mac) - 1] = '\0';
  header->pointCount = input->count;
  
  if (input->count == 0) {
    return sizeof(DeltaCompressedSensor);
  }
  
  // Set base values from first point
  header->baseTimestamp = input->points[0].timestamp;
  header->baseTemp = input->points[0].t;
  header->baseHumidity = input->points[0].h;
  header->basePressure = input->points[0].p;
  
  LOG("[COMPRESSION] Base values - Temp: " + String(header->baseTemp) + 
      ", Humidity: " + String(header->baseHumidity) + 
      ", Pressure: " + String(header->basePressure) + " Pa");
  
  offset += sizeof(DeltaCompressedSensor);
  
  // Calculate deltas for remaining points
  for (int i = 1; i < input->count; i++) {
    if (offset + sizeof(DeltaCompressedPoint) > maxOutputSize) {
      break; // Buffer full
    }
    
    DeltaCompressedPoint* delta = (DeltaCompressedPoint*)(output + offset);
    
    // Calculate deltas
    delta->deltaTemp = (int16_t)((input->points[i].t - input->points[i-1].t) * 100);
    delta->deltaHumidity = (int16_t)((input->points[i].h - input->points[i-1].h) * 100);
    delta->deltaPressure = (int32_t)(input->points[i].p - input->points[i-1].p);
    delta->timeDelta = (uint16_t)((input->points[i].timestamp - input->points[i-1].timestamp) / 1000);
    
    // Debug pressure deltas for first few points
    if (i <= 3) {
      LOG("[COMPRESSION] Point " + String(i) + " - Pressure: " + String(input->points[i].p) + 
          " Pa, Delta: " + String(delta->deltaPressure) + " Pa");
    }
    
    offset += sizeof(DeltaCompressedPoint);
  }
  
  return offset;
}

/**
 * @brief Calculate compression ratio for logging
 * @param originalSize Original data size
 * @param compressedSize Compressed data size
 * @return Compression ratio (0.0 = no compression, 1.0 = 100% compression)
 */
static float calculateCompressionRatio(size_t originalSize, size_t compressedSize) {
  if (originalSize == 0) return 0.0f;
  return 1.0f - ((float)compressedSize / (float)originalSize);
}

// Base64 encoding helper function - SAFER VERSION
static String encodeBase64(const uint8_t* data, size_t length) {
  base64_encodestate state;
  base64_init_encodestate(&state);
  
  String result;
  result.reserve((length * 4) / 3 + 8); // Extra buffer for safety
  
  // Use smaller buffer to prevent stack overflow
  char buffer[128];
  size_t remaining = length;
  const char* input = (const char*)data;
  
  // Encode in chunks to prevent buffer overflow
  while (remaining > 0) {
    size_t chunkSize = (remaining > 64) ? 64 : remaining; // Process max 64 bytes at a time
    
    int encoded = base64_encode_block(input, chunkSize, buffer, &state);
    if (encoded > 0 && encoded < sizeof(buffer)) {
      result += String(buffer, encoded);
    }
    
    input += chunkSize;
    remaining -= chunkSize;
  }
  
  // Finalize encoding
  int encoded = base64_encode_blockend(buffer, &state);
  if (encoded > 0 && encoded < sizeof(buffer)) {
    result += String(buffer, encoded);
  }
  
  return result;
}

static void handleHistory() {
  gHttpRequestsTotal++; gLastHttpActivityMs = millis();
  LOG("[HTTP] GET /history - Free heap: " + String(ESP.getFreeHeap()));

  // Check if we have any history data
  if (gHistoryCount == 0) {
    http.send(200, "application/json", "{\"data\":{},\"serverTime\":" + String(millis()) + "}");
    return;
  }

  // Calculate total size needed for delta compression
  size_t totalSize = sizeof(BinaryHistoryHeader);
  LOG("[COMPRESSION] Header size: " + String(sizeof(BinaryHistoryHeader)) + " bytes");
  
  for (int i = 0; i < gHistoryCount; i++) {
    size_t sensorSize = sizeof(DeltaCompressedSensor);
    if (gHistory[i].count > 1) {
      sensorSize += (gHistory[i].count - 1) * sizeof(DeltaCompressedPoint);
    }
    totalSize += sensorSize;
    LOG("[COMPRESSION] Sensor " + String(i) + ": " + String(gHistory[i].count) + " points, " + String(sensorSize) + " bytes");
  }
  
  // Use the actual calculated size (delta compression is already efficient)
  size_t compressedSize = totalSize;
  
  LOG("[COMPRESSION] Calculated size: " + String(totalSize) + " bytes for " + String(gHistoryCount) + " sensors");
  
  // Safety check with higher limit for improved system
  if (compressedSize > 32768) { // 32KB limit
    http.send(500, "application/json", "{\"error\":\"Data too large for compression\"}");
    return;
  }

  // Allocate buffer for compressed data
  uint8_t* compressedData = (uint8_t*)malloc(compressedSize);
  if (!compressedData) {
    http.send(500, "application/json", "{\"error\":\"Memory allocation failed\"}");
    return;
  }

  size_t offset = 0;
  
  // Determine if we can use delta compression for all sensors
  bool useDeltaCompression = true;
  for (int i = 0; i < gHistoryCount; i++) {
    if (gHistory[i].count < 2) {
      useDeltaCompression = false; // Need at least 2 points for delta compression
      break;
    }
  }

  // Write header with correct version
  BinaryHistoryHeader* header = (BinaryHistoryHeader*)(compressedData + offset);
  header->magic = 0x48495354; // "HIST"
  header->version = useDeltaCompression ? 2 : 1; // FIXED: Use correct version based on compression capability
  header->sensorCount = gHistoryCount;
  header->serverTime = millis();
  offset += sizeof(BinaryHistoryHeader);

  LOG("[COMPRESSION] Using version " + String(header->version) + " compression");

  // Compress each sensor's data
  for (int i = 0; i < gHistoryCount; i++) {
    if (useDeltaCompression && offset + sizeof(DeltaCompressedSensor) <= compressedSize) {
      // Try delta compression first
      size_t remainingSize = compressedSize - offset;
      LOG("[COMPRESSION] Compressing sensor " + String(i) + ", remaining size: " + String(remainingSize));
      size_t compressedSensorSize = compressDelta(&gHistory[i], compressedData + offset, remainingSize);
      LOG("[COMPRESSION] Sensor " + String(i) + " compressed size: " + String(compressedSensorSize));
      
      if (compressedSensorSize > 0) {
        offset += compressedSensorSize;
        continue; // Success, move to next sensor
      }
    }
    
    // Fallback to simple binary format if delta compression fails or disabled
    LOG("[COMPRESSION] Using fallback format for sensor " + String(i));
    
    // Check bounds before writing fallback data
    if (offset + sizeof(BinarySensorHeader) > compressedSize) {
      LOG("[COMPRESSION] Fallback would exceed buffer for sensor " + String(i));
      break;
    }
    
    // Write sensor header
    BinarySensorHeader* sensorHeader = (BinarySensorHeader*)(compressedData + offset);
    strncpy(sensorHeader->mac, gHistory[i].mac.c_str(), sizeof(sensorHeader->mac) - 1);
    sensorHeader->mac[sizeof(sensorHeader->mac) - 1] = '\0';
    sensorHeader->pointCount = gHistory[i].count;
    offset += sizeof(BinarySensorHeader);
    
    // Write data points in original format
    for (int j = 0; j < gHistory[i].count && offset + sizeof(BinaryDataPoint) <= compressedSize; j++) {
      BinaryDataPoint* point = (BinaryDataPoint*)(compressedData + offset);
      point->timestamp = gHistory[i].points[j].timestamp;
      point->temp = isnan(gHistory[i].points[j].t) ? 0xFFFF : (uint16_t)(gHistory[i].points[j].t * 100);
      point->humidity = isnan(gHistory[i].points[j].h) ? 0xFFFF : (uint16_t)(gHistory[i].points[j].h * 100);
      point->pressure = isnan(gHistory[i].points[j].p) ? 0xFFFFFFFF : (uint32_t)gHistory[i].points[j].p;
      offset += sizeof(BinaryDataPoint);
    }
  }

  // Log compression statistics
  size_t originalSize = 0;
  for (int i = 0; i < gHistoryCount; i++) {
    originalSize += sizeof(BinarySensorHeader) + (gHistory[i].count * sizeof(BinaryDataPoint));
  }
  float compressionRatio = calculateCompressionRatio(originalSize, offset);
  LOG("[COMPRESSION] Original: " + String(originalSize) + " bytes, Compressed: " + String(offset) + " bytes, Ratio: " + String(compressionRatio * 100, 1) + "%");

  // Encode to Base64
  String base64Data = encodeBase64(compressedData, offset);
  free(compressedData);

  // Send response with correct version
  String response = "{\"compressed\":true,\"version\":" + String(header->version) + ",\"data\":\"" + base64Data + "\"}";
  
  http.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  http.sendHeader("Connection", "close");
  http.send(200, "application/json", response);
}

// Clear history buffer
static void handleClearHistory() {
  gHttpRequestsTotal++; gLastHttpActivityMs = millis();
  LOG("[HTTP] GET /clear-history - Free heap: " + String(ESP.getFreeHeap()));
  
  // Clear the history buffer
  for (int i = 0; i < kHistoryMaxSensors; ++i) {
    gHistory[i].mac = "";
    gHistory[i].count = 0;
    gHistory[i].writeIndex = 0;
    // Clear the points array
    for (int j = 0; j < kHistoryPoints; ++j) {
      gHistory[i].points[j].timestamp = 0;
      gHistory[i].points[j].t = NAN;
      gHistory[i].points[j].h = NAN;
      gHistory[i].points[j].p = NAN;
    }
  }
  gHistoryCount = 0;
  
  LOG("[History] History buffer cleared");
  http.send(200, "application/json", "{\"status\":\"cleared\",\"message\":\"History buffer cleared successfully\"}");
}

// --- misc
static void handleHealth() {
  gHttpRequestsTotal++; gLastHttpActivityMs = millis();
  LOG("[HTTP] GET /health - Free heap: " + String(ESP.getFreeHeap()));
  
  // Update CPU usage periodically (every 10 seconds)
  unsigned long now = millis();
  if (now - gLastCpuCheck > 10000) {
    gCpuUsage = calculateCpuUsage();
    gLastCpuCheck = now;
  }
  
  String out; out.reserve(250);
  out += F("{\"heap\":"); out += ESP.getFreeHeap();
  out += F(",\"uptime_ms\":"); out += millis();
  out += F(",\"tags\":"); out += gTagCount;
  out += F(",\"req_total\":"); out += gHttpRequestsTotal;
  out += F(",\"data_reqs\":"); out += gHttpDataRequests;
  out += F(",\"ble_seen\":"); out += gBleAdvertsSeen;
  out += F(",\"ap_mode\":"); out += (gApMode ? "true" : "false");
  out += F(",\"cpu_usage\":"); out += String(gCpuUsage, 1);
  if (WiFi.status() == WL_CONNECTED) {
    out += F(",\"ip\":\""); out += WiFi.localIP().toString(); out += F("\"");
  } else if (gApMode) {
    out += F(",\"ip\":\""); out += WiFi.softAPIP().toString(); out += F("\"");
  }
  out += "}";
  http.send(200, "application/json", out);
}
static void handleReloadNames() { loadNamesCsv(); http.send(200, "text/plain", "names reloaded"); }
static void handlePing()        { http.send(200, "text/plain", "OK"); }

// --- Upload page
static fs::File fsUploadFile;
static String   lastUploadName;
static size_t   lastUploadSize = 0;
static volatile bool uploadDone = false;

static void handleUploadForm() { serveFile("/upload.html"); }

static void handleUploadPost() {
  String msg = uploadDone ? ("OK: " + lastUploadName + " (" + String(lastUploadSize) + " bytes)") : "OK";
  uploadDone = false;
  http.send(200, "text/plain", msg);
}

static void handleFileUpload() {
  HTTPUpload& upload = http.upload();
  switch (upload.status) {
    case UPLOAD_FILE_START: {
      String filename = upload.filename;
      if (!filename.startsWith("/")) filename = "/" + filename;
      lastUploadName = filename;
      lastUploadSize = 0;
      if (LittleFS.exists(filename)) LittleFS.remove(filename);
      fsUploadFile = LittleFS.open(filename, "w");
    } break;
    case UPLOAD_FILE_WRITE:
      if (fsUploadFile) {
        fsUploadFile.write(upload.buf, upload.currentSize);
        lastUploadSize += upload.currentSize;
      }
      break;
    case UPLOAD_FILE_END:
      if (fsUploadFile) fsUploadFile.close();
      uploadDone = true;
      break;
    default: break;
  }
}

// --- Simple Wi-Fi config page (/config)
static void handleConfigGet() { serveFile("/config.html"); }

static void handleConfigPost() {
  String ssid = http.arg("ssid");
  String pass = http.arg("password");
  ssid.trim(); pass.trim();

  if (!ssid.length() || !pass.length()) {
    http.send(400, "text/plain", "ssid and password are required");
    return;
  }

  if (!saveWifiProps(ssid, pass)) {
    http.send(500, "text/plain", "Failed to save /wifi.properties");
    return;
  }

  ensureAPStarted();

  WiFi.mode(WIFI_AP_STA);
  WiFi.disconnect(true, true);
  delay(100);
  WiFi.begin(ssid.c_str(), pass.c_str());

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < kStaConnectTimeoutMs) {
    delay(300);
  }

  if (WiFi.status() == WL_CONNECTED) {
    WiFi.softAPdisconnect(true);
    gApMode = false;
    if (MDNS.begin("home")) LOG("[mDNS] http://home.local/");
    http.send(200, "text/plain", "Saved and connected. AP stopped.");
  } else {
    LOG("[WiFi] /config connect timeout; keeping AP up");
    http.send(200, "text/plain",
      "Saved. STA connect failed; AP stays up.\n"
      "Join AP " + gApSsid + " (pwd: " + kApPassword + "), then retry.");
  }
}

// --- List files
static void handleList() {
  String out = "[";
  bool first = true;
  fs::File root = LittleFS.open("/");
  if (!root) {
    http.send(500, "application/json", "{\"error\":\"Failed to open root directory\"}");
    return;
  }
  
  fs::File f = root.openNextFile();
  while (f) {
    if (!first) out += ",";
    first = false;
    out += "\""; out += f.name(); out += "\"";
    f.close(); // Close each file handle
    f = root.openNextFile();
  }
  root.close(); // Close root directory handle
  out += "]";
  http.send(200, "application/json", out);
}

// =============================== setup/loop ==============================
void setup() {
  Serial.begin(115200);
  delay(5000);
  LOG("\nRuuvi TFT NimBLE (LittleFS + HTTP + AP fallback)");
  Serial.printf("Booting home-dash, HAS_TFT=%d\n", (int)HAS_TFT);

#if HAS_TFT
  // Backlight ON (active-LOW)
  pinMode(kBacklightPin, OUTPUT);
  digitalWrite(kBacklightPin, kBacklightOnLevel);

  // Button pins setup
  pinMode(kButtonUpPin, INPUT_PULLUP);
  pinMode(kButtonDownPin, INPUT_PULLUP);
  pinMode(kButtonAPin, INPUT_PULLUP);
  pinMode(kButtonBPin, INPUT_PULLUP);  // GPIO35 with external pullup resistor

  // TFT init
  tft.init();
  tft.setRotation(3);
  tft.invertDisplay(false);
  tft.fillScreen(TFT_BLACK);

  // Full-screen sprite
  spr.setColorDepth(16);
  spr.createSprite(tft.width(), tft.height());
  spr.fillSprite(TFT_BLACK);
  spr.setTextFont(2);
  spr.setTextColor(TFT_WHITE, TFT_BLACK);
  spr.setCursor(0, 0);
  spr.print("Booting...");
  spr.pushSprite(0, 0);
#endif

  // LittleFS mount (format if needed)
  bool fsOK = LittleFS.begin(false);
  if (!fsOK) {
    LOG("[LittleFS] mount failed, formatting...");
    if (!LittleFS.format()) LOG("[LittleFS] format FAILED");
    fsOK = LittleFS.begin(true);
  }
  if (fsOK) {
    LOG("[LittleFS] mounted");
    loadNamesCsv();
  } else {
    LOG("[LittleFS] unavailable");
  }

  // Wi-Fi: try STA first, else AP fallback
  WifiCreds wc = loadWifiProps();
  bool staOK = false;
  if (wc.valid) {
    WiFi.onEvent(onWiFiEvent);
    staOK = tryConnectSTA(wc, kStaConnectTimeoutMs);
  }
  if (!staOK) {
    ensureAPStarted(); // Visit http://192.168.4.1/
  } else {
    // Initialize NTP time synchronization with UTC first
    configTime(0, 0, ntpServer);
    LOG("[NTP] Time synchronization started (UTC)");
    gTimeInitialized = true;
    
    // Detect timezone asynchronously
    detectTimezone();
  }

  // HTTP routes
  http.on("/",             HTTP_GET,  handleRoot);
  http.on("/app.js",       HTTP_GET,  handleStatic);
  http.on("/styles.css",   HTTP_GET,  handleStatic);
  http.on("/data",         HTTP_GET,  handleDataStream);
  http.on("/data_plain",   HTTP_GET,  handleDataPlain);
  http.on("/history",      HTTP_GET,  handleHistory);
  http.on("/clear-history", HTTP_GET, handleClearHistory);
  http.on("/health",       HTTP_GET,  handleHealth);
  http.on("/upload",       HTTP_GET,  handleUploadForm);
  http.on("/upload",       HTTP_POST, handleUploadPost, handleFileUpload);
  http.on("/list",         HTTP_GET,  handleList);
  http.on("/reload-names", HTTP_GET,  handleReloadNames);
  http.on("/ping",         HTTP_GET,  handlePing);
  http.on("/config",       HTTP_GET,  handleConfigGet);
  http.on("/config",       HTTP_POST, handleConfigPost);
  http.onNotFound(handleStatic);
  http.begin();
  LOG("[HTTP] server started on :80");

  // NimBLE scan
  NimBLEDevice::init("");
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(new RuuviScanCB(), /*wantDuplicates=*/true);
  scan->setActiveScan(true);
  scan->setInterval(300);  // More conservative to reduce memory pressure (was 200ms)
  scan->setWindow(100);    // More conservative to reduce memory pressure (was 80ms)
  scan->setDuplicateFilter(false);
  scan->start(0, /*isContinue=*/false, /*restart=*/false);
  LOG("[BLE] scan started");
}

void loop() {
  static unsigned long lastDebugPrint = 0;
  static unsigned long lastHttpCheck = 0;
  
  if (millis() - lastDebugPrint > 30000) {  // Debug print every 30 seconds
    lastDebugPrint = millis();
    LOG("[DEBUG] Loop running - Free heap: " + String(ESP.getFreeHeap()) + 
        " bytes, Min free: " + String(ESP.getMinFreeHeap()) + 
        " bytes, CPU usage: " + String(gCpuUsage, 1) + "%" +
        ", BLE adverts: " + String(gBleAdvertsSeen) +
        ", HTTP requests: " + String(gHttpRequestsTotal));
  }

  // HTTP server watchdog - check if it's responsive
  if (millis() - lastHttpCheck > 5000) {  // Check every 5 seconds
    lastHttpCheck = millis();
    if (millis() - gLastHttpActivityMs > 60000) {  // No HTTP activity for 60 seconds
      LOG("[WARNING] HTTP server appears unresponsive - Last activity: " + String((millis() - gLastHttpActivityMs) / 1000) + "s ago");
      
      // If HTTP server is unresponsive and gDataInFlight is stuck, reset it
      if (gDataInFlight) {
        LOG("[ERROR] gDataInFlight stuck - forcing reset");
        gDataInFlight = false;
      }
    }
    
    // Memory pressure check - if critically low, force garbage collection
    if (ESP.getFreeHeap() < 20000) {  // Less than 20KB free
      LOG("[WARNING] Critical memory pressure - Free heap: " + String(ESP.getFreeHeap()) + " bytes");
      // Force garbage collection by creating and destroying a large string
      String temp = String("Memory cleanup - Free heap: ") + ESP.getFreeHeap();
      temp = "";  // Force deallocation
      LOG("[MEMORY] Forced cleanup - New free heap: " + String(ESP.getFreeHeap()) + " bytes");
    }
  }

  http.handleClient();

  // Save history data periodically
  saveHistoryData();
  
  // Detect timezone periodically if not detected yet
  if (WiFi.status() == WL_CONNECTED && !gTimezoneDetected) {
    static unsigned long lastTimezoneCheck = 0;
    if (millis() - lastTimezoneCheck > 30000) { // Check every 30 seconds
      lastTimezoneCheck = millis();
      detectTimezone();
    }
  }

#if HAS_TFT
  // Continuous button polling for maximum responsiveness
  if (millis() - gLastButtonPollMs >= kButtonPollIntervalMs) {
    gLastButtonPollMs = millis();
    handleButtons();
  }
  
  // Display refresh (separate from button polling)
  if (millis() - gLastDrawMs >= kDrawIntervalMs) {
    gLastDrawMs = millis();
    drawDashboard();
  }
#endif

  // If STA disconnected and not in AP, optionally kick retry (lightweight)
  static unsigned long lastWiFiCheck = 0;
  if (!gApMode && millis() - lastWiFiCheck > 5000) {   // Conservative setting (was 3s, then 10s - now 5s)
    lastWiFiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      WiFi.reconnect();
    }
  }

  delay(2); // yield
}
