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
constexpr uint32_t kUpdateThrottleMs       = 200;        // Minimum time between sensor updates (ms)
constexpr uint32_t kStaConnectTimeoutMs    = 12000;      // WiFi connection timeout (ms)

// === NETWORK CONFIGURATION ===
static const char* kApPassword           = "ruuvi1234"; // SoftAP password (minimum 8 characters)

// === NTP AND TIMEZONE CONFIGURATION ===
const char* ntpServer = "pool.ntp.org";                    // NTP server for time synchronization
const char* timezoneApiUrl = "http://worldtimeapi.org/api/ip"; // Primary API for timezone detection
const char* timezoneApiUrl2 = "http://ipapi.co/json/";     // Alternative API for timezone detection
const char* timezoneApiUrl3 = "http://ip-api.com/json/";   // Third fallback API for timezone detection
static long gTimezoneOffset = 0;                           // Detected timezone offset in seconds
static bool gTimezoneDetected = false;                     // Whether timezone has been detected
static unsigned long gTimezoneRetryCount = 0;              // Number of timezone detection attempts
constexpr unsigned long kMaxTimezoneRetries = 5;           // Maximum retries before fallback

// Manual timezone configuration (set to non-zero to override automatic detection)
// Examples: 3*3600 = UTC+3, -5*3600 = UTC-5, 0 = UTC
// To set your timezone manually, change the value below:
// UTC+1 (Europe): 1*3600 = 3600
// UTC+2 (Europe): 2*3600 = 7200  
// UTC+3 (Europe/Middle East): 3*3600 = 10800
// UTC-5 (US Eastern): -5*3600 = -18000
// UTC-8 (US Pacific): -8*3600 = -28800
static long gManualTimezoneOffset = 0;                     // Manual timezone override (0 = auto-detect)
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
  // Check for manual timezone override first
  if (gManualTimezoneOffset != 0) {
    gTimezoneOffset = gManualTimezoneOffset;
    gTimezoneDetected = true;
    configTime(gTimezoneOffset, 0, ntpServer);
    LOG("[TIMEZONE] Using manual timezone offset: " + String(gTimezoneOffset) + " seconds (" + 
        String(gTimezoneOffset / 3600) + "h " + String((gTimezoneOffset % 3600) / 60) + "m)");
    return;
  }
  
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
  http.setTimeout(10000); // Increased timeout to 10 seconds
  http.addHeader("User-Agent", "ESP32-HomeDash/1.0");
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS); // Follow redirects
  
  int httpCode = http.GET();
  LOG("[TIMEZONE] HTTP response code: " + String(httpCode));
  
  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    LOG("[TIMEZONE] API response length: " + String(payload.length()) + " bytes");
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
          LOG("[TIMEZONE] Invalid offset format: " + offsetStr + " (length: " + String(offsetStr.length()) + ")");
        }
      } else {
        LOG("[TIMEZONE] Could not find offset end in response");
      }
    } else {
      LOG("[TIMEZONE] Could not find utc_offset in response");
      // Try alternative field names
      int altOffsetStart = payload.indexOf("\"utc_offset\":");
      if (altOffsetStart != -1) {
        LOG("[TIMEZONE] Found utc_offset without quotes, trying alternative parsing");
        altOffsetStart += 13; // Skip "utc_offset":
        int altOffsetEnd = payload.indexOf(",", altOffsetStart);
        if (altOffsetEnd == -1) altOffsetEnd = payload.indexOf("}", altOffsetStart);
        if (altOffsetEnd != -1) {
          String altOffsetStr = payload.substring(altOffsetStart, altOffsetEnd);
          altOffsetStr.trim();
          LOG("[TIMEZONE] Alternative offset string: " + altOffsetStr);
        }
      }
    }
  } else {
    LOG("[TIMEZONE] Primary API call failed, code: " + String(httpCode));
    if (httpCode == HTTPC_ERROR_CONNECTION_REFUSED) {
      LOG("[TIMEZONE] Connection refused - check internet connectivity");
    } else if (httpCode == HTTPC_ERROR_CONNECTION_LOST) {
      LOG("[TIMEZONE] Connection lost during request");
    } else if (httpCode == HTTPC_ERROR_NO_HTTP_SERVER) {
      LOG("[TIMEZONE] No HTTP server response");
    } else if (httpCode == HTTPC_ERROR_READ_TIMEOUT) {
      LOG("[TIMEZONE] Read timeout");
    }
    
    // Try alternative API if primary failed
    LOG("[TIMEZONE] Trying alternative API...");
    if (tryAlternativeTimezoneAPI()) {
      LOG("[TIMEZONE] Alternative API succeeded");
      return;
    } else {
      LOG("[TIMEZONE] Alternative API also failed, trying third API...");
      if (tryThirdTimezoneAPI()) {
        LOG("[TIMEZONE] Third API succeeded");
        return;
      } else {
        LOG("[TIMEZONE] All APIs failed");
      }
    }
  }
  
  http.end();
}

/**
 * @brief Try alternative timezone API (ipapi.co)
 * 
 * This function tries the ipapi.co service as a fallback if the primary API fails.
 * It looks for the "utc_offset" field in the response.
 */
static bool tryAlternativeTimezoneAPI() {
  LOG("[TIMEZONE] Trying alternative API: " + String(timezoneApiUrl2));
  
  HTTPClient http;
  http.begin(timezoneApiUrl2);
  http.setTimeout(10000);
  http.addHeader("User-Agent", "ESP32-HomeDash/1.0");
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS); // Follow redirects
  
  int httpCode = http.GET();
  LOG("[TIMEZONE] Alternative API response code: " + String(httpCode));
  
  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    LOG("[TIMEZONE] Alternative API response: " + payload);
    
    // Try to find utc_offset in the response
    int offsetStart = payload.indexOf("\"utc_offset\":\"");
    if (offsetStart != -1) {
      offsetStart += 14; // Skip "utc_offset":""
      int offsetEnd = payload.indexOf("\"", offsetStart);
      if (offsetEnd != -1) {
        String offsetStr = payload.substring(offsetStart, offsetEnd);
        LOG("[TIMEZONE] Alternative API detected offset: " + offsetStr);
        
        // Parse offset format: "+03:00" or "-05:00"
        if (offsetStr.length() >= 6 && (offsetStr[0] == '+' || offsetStr[0] == '-')) {
          int sign = (offsetStr[0] == '+') ? 1 : -1;
          int hours = offsetStr.substring(1, 3).toInt();
          int minutes = offsetStr.substring(4, 6).toInt();
          
          gTimezoneOffset = sign * (hours * 3600 + minutes * 60);
          gTimezoneDetected = true;
          
          LOG("[TIMEZONE] Alternative API set timezone offset: " + String(gTimezoneOffset) + " seconds (" + 
              String(sign * hours) + "h " + String(minutes) + "m)");
          
          // Reconfigure NTP with detected timezone
          configTime(gTimezoneOffset, 0, ntpServer);
          LOG("[TIMEZONE] NTP reconfigured with alternative API timezone");
          http.end();
          return true;
        }
      }
    }
    
    // Try without quotes
    offsetStart = payload.indexOf("\"utc_offset\":");
    if (offsetStart != -1) {
      offsetStart += 13; // Skip "utc_offset":
      int offsetEnd = payload.indexOf(",", offsetStart);
      if (offsetEnd == -1) offsetEnd = payload.indexOf("}", offsetStart);
      if (offsetEnd != -1) {
        String offsetStr = payload.substring(offsetStart, offsetEnd);
        offsetStr.trim();
        offsetStr.replace("\"", ""); // Remove any quotes
        LOG("[TIMEZONE] Alternative API offset (no quotes): " + offsetStr);
        
        if (offsetStr.length() >= 6 && (offsetStr[0] == '+' || offsetStr[0] == '-')) {
          int sign = (offsetStr[0] == '+') ? 1 : -1;
          int hours = offsetStr.substring(1, 3).toInt();
          int minutes = offsetStr.substring(4, 6).toInt();
          
          gTimezoneOffset = sign * (hours * 3600 + minutes * 60);
          gTimezoneDetected = true;
          
          LOG("[TIMEZONE] Alternative API set timezone offset: " + String(gTimezoneOffset) + " seconds (" + 
              String(sign * hours) + "h " + String(minutes) + "m)");
          
          configTime(gTimezoneOffset, 0, ntpServer);
          LOG("[TIMEZONE] NTP reconfigured with alternative API timezone");
          http.end();
          return true;
        }
      }
    }
  }
  
  http.end();
  return false;
}

/**
 * @brief Try third fallback timezone API (ip-api.com)
 * 
 * This function tries the ip-api.com service as a final fallback.
 * It looks for the "timezone" field and converts it to UTC offset.
 */
static bool tryThirdTimezoneAPI() {
  LOG("[TIMEZONE] Trying third API: " + String(timezoneApiUrl3));
  
  HTTPClient http;
  http.begin(timezoneApiUrl3);
  http.setTimeout(10000);
  http.addHeader("User-Agent", "ESP32-HomeDash/1.0");
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  
  int httpCode = http.GET();
  LOG("[TIMEZONE] Third API response code: " + String(httpCode));
  
  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    LOG("[TIMEZONE] Third API response: " + payload);
    
    // Try to find timezone field (e.g., "timezone":"Europe/London")
    int timezoneStart = payload.indexOf("\"timezone\":\"");
    if (timezoneStart != -1) {
      timezoneStart += 12; // Skip "timezone":""
      int timezoneEnd = payload.indexOf("\"", timezoneStart);
      if (timezoneEnd != -1) {
        String timezoneStr = payload.substring(timezoneStart, timezoneEnd);
        LOG("[TIMEZONE] Third API detected timezone: " + timezoneStr);
        
        // Convert common timezone names to UTC offsets
        // This is a simplified mapping - in production you'd want a more comprehensive list
        if (timezoneStr.indexOf("Europe/London") != -1 || timezoneStr.indexOf("Europe/Dublin") != -1) {
          gTimezoneOffset = 0; // UTC+0 (or UTC+1 in summer, but we'll use standard time)
        } else if (timezoneStr.indexOf("Europe/") != -1) {
          gTimezoneOffset = 1 * 3600; // UTC+1 for most of Europe
        } else if (timezoneStr.indexOf("America/New_York") != -1 || timezoneStr.indexOf("America/Toronto") != -1) {
          gTimezoneOffset = -5 * 3600; // UTC-5 (Eastern Time)
        } else if (timezoneStr.indexOf("America/Chicago") != -1) {
          gTimezoneOffset = -6 * 3600; // UTC-6 (Central Time)
        } else if (timezoneStr.indexOf("America/Denver") != -1) {
          gTimezoneOffset = -7 * 3600; // UTC-7 (Mountain Time)
        } else if (timezoneStr.indexOf("America/Los_Angeles") != -1) {
          gTimezoneOffset = -8 * 3600; // UTC-8 (Pacific Time)
        } else if (timezoneStr.indexOf("Asia/") != -1) {
          gTimezoneOffset = 8 * 3600; // UTC+8 for most of Asia (simplified)
        } else {
          // Default fallback based on timezone string
          gTimezoneOffset = 0; // UTC as fallback
        }
        
        gTimezoneDetected = true;
        LOG("[TIMEZONE] Third API set timezone offset: " + String(gTimezoneOffset) + " seconds (" + 
            String(gTimezoneOffset / 3600) + "h)");
        
        configTime(gTimezoneOffset, 0, ntpServer);
        LOG("[TIMEZONE] NTP reconfigured with third API timezone");
        http.end();
        return true;
      }
    }
  }
  
  http.end();
  return false;
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

// History functionality removed: no history buffers, compression, or endpoints

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

  TaskStatus_t* taskStatusArray = (TaskStatus_t*)malloc(taskCount * sizeof(TaskStatus_t));
  if (!taskStatusArray) return 0.0f;

  unsigned long totalRuntime = 0;
  UBaseType_t actualTaskCount = uxTaskGetSystemState(taskStatusArray, taskCount, &totalRuntime);
  if (actualTaskCount == 0 || totalRuntime == 0) {
    free(taskStatusArray);
    return 0.0f;
  }

  // Find current task runtime counter
  unsigned long currentTaskRuntime = 0;
  for (UBaseType_t i = 0; i < actualTaskCount; i++) {
    if (taskStatusArray[i].xHandle == currentTask) {
      currentTaskRuntime = taskStatusArray[i].ulRunTimeCounter;
      break;
    }
  }

  free(taskStatusArray);

  // Static snapshot across calls to compute deltas
  static unsigned long prevTotalRuntime = 0;
  static unsigned long prevTaskRuntime = 0;

  if (prevTotalRuntime == 0 || prevTaskRuntime == 0) {
    // First call: initialize baseline
    prevTotalRuntime = totalRuntime;
    prevTaskRuntime = currentTaskRuntime;
    return 0.0f;
  }

  // Compute deltas with wrap protection
  unsigned long deltaTotal = (totalRuntime >= prevTotalRuntime)
                               ? (totalRuntime - prevTotalRuntime)
                               : (UINT32_MAX - prevTotalRuntime + 1UL + totalRuntime);
  unsigned long deltaTask = (currentTaskRuntime >= prevTaskRuntime)
                               ? (currentTaskRuntime - prevTaskRuntime)
                               : (UINT32_MAX - prevTaskRuntime + 1UL + currentTaskRuntime);

  prevTotalRuntime = totalRuntime;
  prevTaskRuntime = currentTaskRuntime;

  if (deltaTotal == 0) return 0.0f;

  // Percentage of CPU time consumed by this task in the last interval
  float cpuUsage = (deltaTask * 100.0f) / (float)deltaTotal;
  // Clamp
  if (cpuUsage < 0.0f) cpuUsage = 0.0f;
  if (cpuUsage > 100.0f) cpuUsage = 100.0f;
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
// saveHistoryData() removed

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

// Efficient friendly name lookup without allocating temporary Strings
static void getFriendlyNameCStr(const String& mac, char* out, size_t outSize) {
  if (!out || outSize == 0) return;
  out[0] = '\0';
  if (mac.length() == 0) {
    strncpy(out, "Unknown", outSize - 1);
    out[outSize - 1] = '\0';
    return;
  }
  for (int i = 0; i < gNamesCount; ++i) {
    // Case-insensitive compare to avoid allocating uppercase copies
    if (gNames[i].mac.equalsIgnoreCase(mac)) {
      strncpy(out, gNames[i].name.c_str(), outSize - 1);
      out[outSize - 1] = '\0';
      return;
    }
  }
  // Fallback to MAC string
  strncpy(out, mac.c_str(), outSize - 1);
  out[outSize - 1] = '\0';
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

// (removed) /data_plain endpoint

// === DELTA COMPRESSION FUNCTIONS ===

/**
 * @brief Compress sensor data using delta compression
 * @param input Raw sensor data
 * @param output Compressed data buffer
 * @param maxOutputSize Maximum output buffer size
 * @return Actual compressed size, or 0 on error
 */
static size_t compressDelta(const SensorHistory* /*input*/, uint8_t* /*output*/, size_t /*maxOutputSize*/) {
  return 0;
}

/**
 * @brief Calculate compression ratio for logging
 * @param originalSize Original data size
 * @param compressedSize Compressed data size
 * @return Compression ratio (0.0 = no compression, 1.0 = 100% compression)
 */
static float calculateCompressionRatio(size_t /*originalSize*/, size_t /*compressedSize*/) {
  return 0.0f;
}

// Base64 encoding helper function - SAFER VERSION
static String encodeBase64(const uint8_t* /*data*/, size_t /*length*/) {
  return String();
}

static void handleHistory() {
  gHttpRequestsTotal++; gLastHttpActivityMs = millis();
  http.send(404, "text/plain", "History disabled");
}

static void handleClearHistory() {
  gHttpRequestsTotal++; gLastHttpActivityMs = millis();
  http.send(404, "text/plain", "History disabled");
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

// --- Names endpoints
static void handleNamesGet() {
  gHttpRequestsTotal++; gLastHttpActivityMs = millis();
  http.sendHeader("Cache-Control","no-cache, no-store, must-revalidate");
  if (!LittleFS.exists("/names.csv")) {
    http.send(200, "text/plain", "MAC,Name\n");
    return;
  }
  fs::File f = LittleFS.open("/names.csv", "r");
  if (!f) { http.send(500, "text/plain", "Failed to open names.csv"); return; }
  String content; content.reserve(1024);
  while (f.available()) content += f.readStringUntil('\n') + "\n";
  f.close();
  http.send(200, "text/plain", content);
}

static void handleNamesPost() {
  gHttpRequestsTotal++; gLastHttpActivityMs = millis();
  // Expect raw body text as names.csv content
  if (!http.hasArg("plain")) { http.send(400, "text/plain", "Missing body"); return; }
  String body = http.arg("plain");
  // Basic size guard
  if (body.length() > 8192) { http.send(413, "text/plain", "Payload too large"); return; }
  fs::File f = LittleFS.open("/names.csv", "w");
  if (!f) { http.send(500, "text/plain", "Failed to write names.csv"); return; }
  f.print(body);
  f.close();
  // Reload names into memory
  loadNamesCsv();
  http.send(200, "text/plain", "Saved");
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
  // (removed) http.on("/data_plain",   HTTP_GET,  handleDataPlain);
  http.on("/health",       HTTP_GET,  handleHealth);
  http.on("/upload",       HTTP_GET,  handleUploadForm);
  http.on("/upload",       HTTP_POST, handleUploadPost, handleFileUpload);
  http.on("/list",         HTTP_GET,  handleList);
  http.on("/reload-names", HTTP_GET,  handleReloadNames);
  http.on("/names",        HTTP_GET,  handleNamesGet);
  http.on("/names",        HTTP_POST, handleNamesPost);
  http.on("/ping",         HTTP_GET,  handlePing);
  http.on("/config",       HTTP_GET,  handleConfigGet);
  http.on("/config",       HTTP_POST, handleConfigPost);
  http.onNotFound(handleStatic);
  http.begin();
  LOG("[HTTP] server started on :80");

  // NimBLE scan
  NimBLEDevice::init("");
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(new RuuviScanCB(), /*wantDuplicates=*/false);
  scan->setActiveScan(true);
  scan->setInterval(300);  // Balanced scan rate for detection vs memory
  scan->setWindow(100);    // Wider window for better detection
  scan->setDuplicateFilter(false); // Allow duplicates but no callback duplicates
  scan->start(0, /*isContinue=*/false, /*restart=*/false);
  LOG("[BLE] scan started (balanced detection)");
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

  // History functionality removed
  
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
