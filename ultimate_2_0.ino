#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <ESP32Servo.h>
#include <math.h>
#include <esp_system.h>

// =====================================================
// SMART RAIN SHIELD  (ultimate 2.0)
//
// 2.0 changes on top of the corrected version:
//  - Servo holds position when idle (detach off by default).
//  - Weather decision applied only in Weather Mode; when weather data
//    is stale or missing, Weather Mode behaves like Sensor Mode.
//  - Rain sensor must stay dry for RAIN_CLEAR_HOLD_MS before the
//    shield reopens (stops flapping from leftover droplets).
//  - Dashboard status shows real shield state (FOLDED / OPEN / moving).
//  - "Internet" row relabelled "Home Wi-Fi" (it only shows association).
//  - Readable weather HTTP error text.
//  - Reset reason + heap/stack diagnostics on Serial.
// ESP32 + DIGITAL RAIN SENSOR + MG90S + OPEN-METEO
//
// Main fixes in this version:
//  - Weather fetch runs in its own FreeRTOS task, so the servo,
//    rain sensor and web server never freeze during HTTPS requests.
//  - Servo re-sends its position when a command arrives, even if the
//    code thinks it is already there (fixes "servo not responding"
//    after a power glitch). Optional detach when idle.
//  - Servo updates are throttled (smoother, less jitter).
//  - Wi-Fi reconnect no longer calls WiFi.disconnect() every 10 s
//    (that was dropping the RAIN SENSOR hotspot).
//  - mDNS starts after the home Wi-Fi gets an IP.
//  - A failed weather fetch keeps the last good data (marked stale
//    after 30 min) instead of wiping it.
//  - Rain sensor folds the shield once in Mobile Mode too.
//  - Smaller JSON buffer, thread-safe weather data.
//
// HARDWARE: power the servo from a separate 5V supply (>=1A), join
// its GND with the ESP32 GND, and add a 470-1000 uF capacitor across
// servo 5V/GND. This is the most common cause of a servo that
// "sometimes" does not respond.
// =====================================================

// ---------------- PIN DEFINITIONS ---------------------
#define RAIN_SENSOR_PIN 27
#define SERVO_PIN       13

// ---------------- RAIN SENSOR LOGIC -------------------
// HIGH = DRY
// LOW  = RAIN / WET
#define RAIN_DETECTED LOW

// ---------------- SERVO SETTINGS ----------------------
const int OPEN_ANGLE = 80;
const int FOLD_ANGLE = 0;
const unsigned long FULL_MOVE_TIME = 1500;
const int SERVO_MIN_US = 500;
const int SERVO_MAX_US = 2400;
const unsigned long SERVO_UPDATE_MS = 15;          // max one servo write per 15 ms while moving
const bool SERVO_DETACH_WHEN_IDLE = false;         // false = servo keeps holding position (recommended under load)
const unsigned long SERVO_IDLE_DETACH_MS = 1000;   // detach this long after a move ends

// In Mobile Mode, fold once automatically when rain starts.
// You can still press OPEN afterwards.
const bool MOBILE_RAIN_OVERRIDE = true;

// ---------------- ESP32 ACCESS POINT ------------------
// CHANGE THESE PASSWORDS to something private.
const char* AP_SSID = "RAIN SENSOR";
const char* AP_PASSWORD = "12345678";
IPAddress AP_IP(192, 168, 4, 1);
IPAddress AP_GATEWAY(192, 168, 4, 1);
IPAddress AP_SUBNET(255, 255, 255, 0);

// ---------------- HOME / PHONE HOTSPOT WI-FI ----------
const char* HOME_WIFI_SSID = "Hayah";
const char* HOME_WIFI_PASSWORD = "12345678";

// ---------------- DEFAULT WEATHER LOCATION -----------
const double DEFAULT_LATITUDE = 10.607028;
const double DEFAULT_LONGITUDE = 76.199542;

// ---------------- WEATHER SETTINGS --------------------
const unsigned long WEATHER_UPDATE_INTERVAL = 10UL * 60UL * 1000UL;
const unsigned long WEATHER_RETRY_INTERVAL  = 30UL * 1000UL;
const unsigned long WEATHER_STALE_MS        = 30UL * 60UL * 1000UL;
const unsigned long WEATHER_HTTP_TIMEOUT    = 10000;
const unsigned long WIFI_RECONNECT_INTERVAL = 60000;
unsigned long lastWiFiReconnectAttempt = 0;
unsigned long lastSerialReport = 0;

// ---------------- WEB SERVER --------------------------
WebServer server(80);
Preferences preferences;
Servo myServo;

// ---------------- CONTROL MODES -----------------------
enum ControlMode {
  SENSOR_MODE,
  MOBILE_MODE,
  WEATHER_MODE
};

ControlMode controlMode = SENSOR_MODE;

// ---------------- SERVO STATE -------------------------
float currentAngle = OPEN_ANGLE;
float startAngle = OPEN_ANGLE;
float targetAngle = OPEN_ANGLE;
unsigned long moveStartTime = 0;
unsigned long moveDuration = 0;
unsigned long lastServoWrite = 0;
unsigned long lastServoActivity = 0;
bool servoMoving = false;
bool servoAttached = false;

// ---------------- SENSOR STATE ------------------------
bool rainDetected = false;
int currentDO = HIGH;
int lastRawState = HIGH;
int stableCount = 0;
unsigned long lastSensorCheck = 0;
const unsigned long SENSOR_INTERVAL = 50;
const int REQUIRED_STABLE_READINGS = 5;
const unsigned long RAIN_CLEAR_HOLD_MS = 2UL * 60UL * 1000UL;   // must stay dry this long before reopening
unsigned long dryStableSince = 0;

// ---------------- WEATHER STATE -----------------------
struct WeatherData {
  float temperature = NAN;
  float feelsLike = NAN;
  float humidity = NAN;
  float cloudCover = NAN;
  float precipitation = NAN;
  float rain = NAN;
  float showers = NAN;
  float windSpeed = NAN;
  float windDirection = NAN;
  float windGust = NAN;
  float pressure = NAN;
  float uv = NAN;
  int code = -1;
  int next3hProbability = -1;
  float next3hPrecipitation = NAN;
  float next3hRain = NAN;
  float next3hShowers = NAN;
  bool risk = false;
  String likelyRainTime = "No significant rain forecast";
  String decision = "MONITORING WEATHER";
  String description = "Weather unavailable";
  String sunrise = "--:--";
  String sunset = "--:--";
};

// Shared between the weather task (core 0) and the main loop (core 1).
// Anything non-trivial is protected by dataMutex.
SemaphoreHandle_t dataMutex = NULL;
WeatherData wx;
String weatherError = "";
double weatherLatitude = DEFAULT_LATITUDE;
double weatherLongitude = DEFAULT_LONGITUDE;

volatile bool weatherEverOk = false;
volatile bool weatherRisk = false;
volatile bool weatherDirty = false;
volatile bool forceWeatherFetch = false;
volatile bool lastAttemptOk = false;
volatile bool weatherAttempted = false;
volatile unsigned long lastWeatherUpdate = 0;
volatile unsigned long lastWeatherAttempt = 0;
volatile bool mdnsNeedsStart = false;

// =====================================================
// HELPER FUNCTIONS
// =====================================================

inline void lockData()   { if (dataMutex) xSemaphoreTake(dataMutex, portMAX_DELAY); }
inline void unlockData() { if (dataMutex) xSemaphoreGive(dataMutex); }

void setWeatherError(const String& msg) {
  lockData();
  weatherError = msg;
  unlockData();
  if (msg.length()) Serial.println("[Weather] " + msg);
}

bool weatherFresh() {
  return weatherEverOk && (millis() - lastWeatherUpdate < WEATHER_STALE_MS);
}

String modeName() {
  if (controlMode == SENSOR_MODE) return "SENSOR MODE";
  if (controlMode == MOBILE_MODE) return "MOBILE MODE";
  return "WEATHER MODE";
}

String jsonEscape(const String& input) {
  String out;
  out.reserve(input.length() + 16);

  for (size_t i = 0; i < input.length(); i++) {
    char c = input[i];

    if (c == '\\') out += "\\\\";
    else if (c == '"') out += "\\\"";
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if (c == '\t') out += "\\t";
    else out += c;
  }

  return out;
}

String numberOrNull(float value, int decimals = 1) {
  if (isnan(value)) return "null";
  return String(value, decimals);
}

String timeOnly(const String& iso) {
  int t = iso.indexOf('T');
  if (t >= 0 && iso.length() >= (size_t)(t + 6)) {
    return iso.substring(t + 1, t + 6);
  }
  return iso;
}

String weatherTextFromCode(int code) {
  switch (code) {
    case 0: return "Clear sky";
    case 1: return "Mainly clear";
    case 2: return "Partly cloudy";
    case 3: return "Overcast";
    case 45:
    case 48: return "Fog";
    case 51:
    case 53:
    case 55: return "Drizzle";
    case 56:
    case 57: return "Freezing drizzle";
    case 61:
    case 63:
    case 65: return "Rain";
    case 66:
    case 67: return "Freezing rain";
    case 71:
    case 73:
    case 75:
    case 77: return "Snow";
    case 80:
    case 81:
    case 82: return "Rain showers";
    case 85:
    case 86: return "Snow showers";
    case 95: return "Thunderstorm";
    case 96:
    case 99: return "Thunderstorm with hail";
    default: return "Unknown weather";
  }
}

bool codeMeansRain(int code) {
  return (code >= 51 && code <= 67) ||
         (code >= 80 && code <= 82) ||
         code == 95 || code == 96 || code == 99;
}

// Works on a local WeatherData, so no lock is needed.
void computeWeatherDecision(WeatherData& d) {
  bool rainNow = false;
  bool rainLikely = false;

  if (!isnan(d.precipitation) && d.precipitation > 0.1f) rainNow = true;
  if (!isnan(d.rain) && d.rain > 0.1f) rainNow = true;
  if (!isnan(d.showers) && d.showers > 0.1f) rainNow = true;
  if (codeMeansRain(d.code)) rainNow = true;

  if (d.next3hProbability >= 60) rainLikely = true;
  if (!isnan(d.next3hRain) && d.next3hRain >= 0.2f) rainLikely = true;
  if (!isnan(d.next3hShowers) && d.next3hShowers >= 0.2f) rainLikely = true;
  if (!isnan(d.next3hPrecipitation) && d.next3hPrecipitation >= 0.2f) rainLikely = true;

  if (rainNow) d.decision = "RAIN NOW";
  else if (rainLikely) d.decision = "RAIN LIKELY";
  else d.decision = "WEATHER LOOKS SAFE";

  d.risk = rainNow || rainLikely;
}

// =====================================================
// SERVO MOVEMENT  (only ever called from the main loop)
// =====================================================

void servoAttachIfNeeded() {
  if (!servoAttached) {
    myServo.setPeriodHertz(50);
    myServo.attach(SERVO_PIN, SERVO_MIN_US, SERVO_MAX_US);
    servoAttached = true;
  }
}

void updateServoMovement() {
  unsigned long now = millis();

  if (servoMoving) {
    unsigned long elapsed = now - moveStartTime;

    if (moveDuration == 0 || elapsed >= moveDuration) {
      currentAngle = targetAngle;
      myServo.write((int)roundf(currentAngle));
      servoMoving = false;
      lastServoWrite = now;
      lastServoActivity = now;
      return;
    }

    if (now - lastServoWrite < SERVO_UPDATE_MS) return;  // throttle
    lastServoWrite = now;

    float progress = (float)elapsed / (float)moveDuration;
    float smoothProgress = progress * progress * (3.0f - 2.0f * progress);
    currentAngle = startAngle + ((targetAngle - startAngle) * smoothProgress);
    myServo.write((int)roundf(currentAngle));
    return;
  }

  // Idle: release the servo after a short while.
  if (SERVO_DETACH_WHEN_IDLE && servoAttached &&
      now - lastServoActivity >= SERVO_IDLE_DETACH_MS) {
    myServo.detach();
    servoAttached = false;
  }
}

void moveServo(int newAngle) {
  newAngle = constrain(newAngle, FOLD_ANGLE, OPEN_ANGLE);
  updateServoMovement();

  // Already heading to this target.
  if (servoMoving && newAngle == (int)roundf(targetAngle)) return;

  // Already at this angle (as far as the code knows). Re-send the position
  // anyway, in case the servo lost it after a power dip or was pushed.
  if (!servoMoving && newAngle == (int)roundf(currentAngle)) {
    servoAttachIfNeeded();
    myServo.write(newAngle);
    lastServoActivity = millis();
    return;
  }

  servoAttachIfNeeded();

  startAngle = currentAngle;
  targetAngle = (float)newAngle;
  moveStartTime = millis();
  lastServoWrite = 0;

  float distance = fabsf(targetAngle - startAngle);
  moveDuration = (unsigned long)(FULL_MOVE_TIME * distance / 80.0f);
  if (moveDuration < 100) moveDuration = 100;
  servoMoving = true;
}

// =====================================================
// RAIN SENSOR
// =====================================================

void applyAutomaticDecision() {
  if (controlMode == SENSOR_MODE) {
    moveServo(rainDetected ? FOLD_ANGLE : OPEN_ANGLE);
  } else if (controlMode == WEATHER_MODE) {
    // Physical rain always has priority.
    if (rainDetected) {
      moveServo(FOLD_ANGLE);
    } else if (weatherFresh()) {
      moveServo(weatherRisk ? FOLD_ANGLE : OPEN_ANGLE);
    } else {
      moveServo(OPEN_ANGLE);   // weather stale or missing: behave like Sensor Mode
    }
  }
  // Mobile Mode: manual control only (see updateRainSensor for the rain override).
}

void updateRainSensor() {
  unsigned long now = millis();
  if (now - lastSensorCheck < SENSOR_INTERVAL) return;
  lastSensorCheck = now;

  int rawState = digitalRead(RAIN_SENSOR_PIN);
  currentDO = rawState;

  if (rawState == lastRawState) {
    stableCount++;
  } else {
    lastRawState = rawState;
    stableCount = 0;
    dryStableSince = 0;
  }

  if (stableCount >= REQUIRED_STABLE_READINGS) {
    bool detected = (rawState == RAIN_DETECTED);
    if (detected) dryStableSince = 0;
    else if (dryStableSince == 0) dryStableSince = now;
    // Rain starts at once; rain only clears after the dry-hold time.
    bool allow = detected || (now - dryStableSince >= RAIN_CLEAR_HOLD_MS);

    if (detected != rainDetected && allow) {
      rainDetected = detected;
      if (controlMode == MOBILE_MODE) {
        if (MOBILE_RAIN_OVERRIDE && rainDetected) moveServo(FOLD_ANGLE);
      } else {
        applyAutomaticDecision();
      }
    }
  }
}

// =====================================================
// WEATHER API  (runs inside weatherTask on core 0)
// =====================================================

bool fetchWeather() {
  weatherAttempted = true;
  lastWeatherAttempt = millis();
  lastAttemptOk = false;

  if (WiFi.status() != WL_CONNECTED) {
    setWeatherError("Home Wi-Fi is not connected");
    return false;
  }

  Serial.printf("[Weather] Fetching... heap=%u stackFree=%u\n",
                (unsigned)ESP.getFreeHeap(), (unsigned)uxTaskGetStackHighWaterMark(NULL));

  double lat, lon;
  lockData();
  lat = weatherLatitude;
  lon = weatherLongitude;
  unlockData();

  String url = "https://api.open-meteo.com/v1/forecast?latitude=";
  url += String(lat, 6);
  url += "&longitude=";
  url += String(lon, 6);
  url += "&current=temperature_2m,relative_humidity_2m,apparent_temperature,is_day,precipitation,rain,showers,weather_code,cloud_cover,pressure_msl,wind_speed_10m,wind_direction_10m,wind_gusts_10m";
  url += "&hourly=precipitation_probability,precipitation,rain,showers,weather_code";
  url += "&daily=uv_index_max,precipitation_probability_max,precipitation_sum,rain_sum,showers_sum,sunrise,sunset";
  url += "&forecast_hours=6&forecast_days=1&timezone=auto";

  WiFiClientSecure client;
  client.setInsecure();   // public weather data only; certificate is not verified

  HTTPClient http;
  http.setTimeout(WEATHER_HTTP_TIMEOUT);

  if (!http.begin(client, url)) {
    setWeatherError("Could not start weather request");
    return false;
  }

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    setWeatherError("Weather API error: " + String(httpCode) + " " + http.errorToString(httpCode));
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();

  DynamicJsonDocument doc(12288);
  DeserializationError error = deserializeJson(doc, payload);
  payload = "";   // free memory early

  if (error) {
    setWeatherError("Weather JSON parsing failed: " + String(error.c_str()));
    return false;
  }

  JsonObject current = doc["current"];
  if (current.isNull()) {
    setWeatherError("Weather response has no current data");
    return false;
  }

  // Parse everything into a local struct first.
  WeatherData d;
  d.temperature   = current["temperature_2m"] | NAN;
  d.feelsLike     = current["apparent_temperature"] | NAN;
  d.humidity      = current["relative_humidity_2m"] | NAN;
  d.cloudCover    = current["cloud_cover"] | NAN;
  d.precipitation = current["precipitation"] | NAN;
  d.rain          = current["rain"] | NAN;
  d.showers       = current["showers"] | NAN;
  d.windSpeed     = current["wind_speed_10m"] | NAN;
  d.windDirection = current["wind_direction_10m"] | NAN;
  d.windGust      = current["wind_gusts_10m"] | NAN;
  d.pressure      = current["pressure_msl"] | NAN;
  d.code          = current["weather_code"] | -1;
  d.description   = weatherTextFromCode(d.code);

  JsonObject daily = doc["daily"];
  if (!daily.isNull()) {
    d.uv = daily["uv_index_max"][0] | NAN;
    const char* sunrise = daily["sunrise"][0] | "";
    const char* sunset  = daily["sunset"][0] | "";
    d.sunrise = timeOnly(String(sunrise));
    d.sunset  = timeOnly(String(sunset));
  }

  d.next3hPrecipitation = 0.0f;
  d.next3hRain = 0.0f;
  d.next3hShowers = 0.0f;

  JsonArray hourlyTimes   = doc["hourly"]["time"];
  JsonArray hourlyProb    = doc["hourly"]["precipitation_probability"];
  JsonArray hourlyPrec    = doc["hourly"]["precipitation"];
  JsonArray hourlyRain    = doc["hourly"]["rain"];
  JsonArray hourlyShowers = doc["hourly"]["showers"];

  int count = hourlyTimes.size();
  int maxProbability = 0;
  bool foundRainTime = false;

  // First hourly entry = current hour, so this covers the current hour
  // plus the next three hours.
  for (int i = 0; i < count && i < 4; i++) {
    int probability = hourlyProb[i] | 0;
    float precipitation = hourlyPrec[i] | 0.0f;
    float rain = hourlyRain[i] | 0.0f;
    float showers = hourlyShowers[i] | 0.0f;

    if (probability > maxProbability) maxProbability = probability;
    d.next3hPrecipitation += precipitation;
    d.next3hRain += rain;
    d.next3hShowers += showers;

    if (!foundRainTime && probability >= 60) {
      const char* t = hourlyTimes[i] | "";
      d.likelyRainTime = timeOnly(String(t));
      foundRainTime = true;
    }
  }
  d.next3hProbability = maxProbability;

  computeWeatherDecision(d);

  // Commit to the shared data in one short locked step.
  lockData();
  wx = d;
  weatherError = "";
  unlockData();

  lastWeatherUpdate = millis();
  weatherRisk = d.risk;
  weatherEverOk = true;
  lastAttemptOk = true;
  weatherDirty = true;   // main loop will apply the servo decision

  Serial.println("[Weather] OK: " + d.description + ", " +
                 String(d.temperature, 1) + " C, decision: " + d.decision);
  return true;
}

void weatherTask(void* param) {
  for (;;) {
    if (WiFi.status() == WL_CONNECTED) {
      unsigned long interval = lastAttemptOk ? WEATHER_UPDATE_INTERVAL : WEATHER_RETRY_INTERVAL;
      bool due = forceWeatherFetch || !weatherAttempted ||
                 (millis() - lastWeatherAttempt >= interval);
      if (due) {
        forceWeatherFetch = false;
        fetchWeather();
      }
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

// =====================================================
// WIFI
// =====================================================

void startAccessPoint() {
  WiFi.softAPConfig(AP_IP, AP_GATEWAY, AP_SUBNET);

  // Note: the AP and the station share one radio. If your hotspot is not on
  // channel 6, the AP will move to the hotspot's channel.
  bool apStarted = WiFi.softAP(AP_SSID, AP_PASSWORD, 6, false, 4);

  if (apStarted) {
    Serial.println("RainShield Wi-Fi AP started successfully.");
    Serial.print("SSID: ");
    Serial.println(AP_SSID);
    Serial.print("AP IP: ");
    Serial.println(WiFi.softAPIP());
  } else {
    Serial.println("ERROR: RainShield Wi-Fi AP failed to start!");
  }
}

bool homeWiFiConfigured() {
  String ssid = HOME_WIFI_SSID;
  String pass = HOME_WIFI_PASSWORD;

  if (ssid.length() == 0 || pass.length() == 0) return false;
  if (ssid == "YOUR_WIFI_NAME" || pass == "YOUR_WIFI_PASSWORD") return false;
  return true;
}

void connectHomeWiFi() {
  if (!homeWiFiConfigured()) {
    Serial.println("Set HOME_WIFI_SSID and HOME_WIFI_PASSWORD in the code first. Hotspot remains available.");
    return;
  }
  Serial.print("Connecting to Wi-Fi: ");
  Serial.println(HOME_WIFI_SSID);
  WiFi.begin(HOME_WIFI_SSID, HOME_WIFI_PASSWORD);
}

void onWiFiEvent(arduino_event_id_t event) {
  if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    mdnsNeedsStart = true;   // handled in loop()
  }
}

// =====================================================
// HTTP HANDLERS
// =====================================================

const char webpage[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<meta name="theme-color" content="#071426">
<title>RainShield</title>
<style>
*{box-sizing:border-box}
html{background:#071426}
body{margin:0;min-height:100vh;padding:16px;font-family:Arial,sans-serif;color:#f1f5f9;background:linear-gradient(135deg,#071426,#12304b)}
main{width:100%;max-width:700px;margin:auto}
header{text-align:center;padding:12px 0 20px}
header h1{margin:0;font-size:32px;letter-spacing:2px}
header p{margin:8px 0 0;color:#9fb3c8;font-size:15px}
.card{background:rgba(255,255,255,.07);border:1px solid rgba(255,255,255,.12);border-radius:22px;padding:20px;margin-bottom:16px;box-shadow:0 10px 30px rgba(0,0,0,.16)}
.mode-row{display:flex;justify-content:space-between;align-items:center;gap:15px}
.label{color:#9fb3c8;font-size:14px;text-transform:uppercase;letter-spacing:1px;margin-bottom:8px}
.mode{font-size:25px;font-weight:700}
.badge{display:inline-block;padding:8px 12px;border-radius:999px;background:#1597ff;font-size:12px;font-weight:700}
.badge.mobile{background:#64748b}
.badge.weather{background:#8b5cf6}
.angle{text-align:center;font-size:64px;font-weight:700;margin:4px 0 12px}
.degree{font-size:.55em}
.track{height:12px;background:#334155;border-radius:999px;overflow:hidden}
.progress{height:100%;width:0;background:#1597ff;border-radius:999px;transition:width .12s linear}
.scale{text-align:center;color:#9fb3c8;font-size:13px;margin:10px 0 0}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:12px}
button,select,input{width:100%;border:0;border-radius:14px;font:inherit}
button{padding:15px;color:white;background:#263b53;font-weight:700;cursor:pointer}
button.active{background:#1597ff}
button.open{background:#22a861}
button.fold{background:#ef4c3e}
button:disabled{opacity:.45;cursor:not-allowed}
select,input{padding:14px 15px;background:#17283b;color:#f1f5f9;border:1px solid #3b5067;outline:none}
.status-box{padding:18px;text-align:center;border-radius:16px;background:#263b53;font-weight:700;font-size:20px}
.status-box.rain{background:#5b2730}
.row{display:flex;justify-content:space-between;gap:12px;padding:11px 0;border-bottom:1px solid rgba(255,255,255,.08)}
.row:last-child{border-bottom:0}
.row span:first-child{color:#aebdcd}
.row span:last-child{text-align:right;font-weight:600}
.weather-icon{text-align:center;font-size:50px;margin:5px 0}
.weather-name{text-align:center;font-size:20px;margin-bottom:12px}
.prob{height:10px;background:#334155;border-radius:999px;overflow:hidden;margin-top:8px}
.prob>div{height:100%;background:#1597ff;width:0}
.decision{text-align:center;padding:14px;border-radius:14px;background:#263b53;font-weight:700;margin-top:12px}
.note{color:#9fb3c8;font-size:13px;line-height:1.7}
.connection{text-align:center;padding:13px;border-radius:14px;background:#163c2c;margin-bottom:16px}
.connection.offline{background:#5b2730}
.small{font-size:12px;color:#9fb3c8;margin-top:8px}
.footer{text-align:center;color:#8da0b4;font-size:12px;padding:8px 0 30px}
@media(max-width:500px){body{padding:10px}.card{padding:16px}.grid{grid-template-columns:1fr}.angle{font-size:54px}.mode{font-size:21px}header h1{font-size:27px}}
</style>
</head>
<body>
<main>
<header>
<h1>RainShield</h1>
<p>Smart Rain Protection System</p>
</header>

<section class="card">
<div class="mode-row">
<div>
<div class="label">System Status</div>
<div id="systemStatus" class="mode">SENSOR: FOLD</div>
</div>
<span id="modeBadge" class="badge">AUTO</span>
</div>
<div class="small" id="modeLine">Mode: SENSOR | Servo: 0&deg;</div>
</section>

<section class="card">
<div class="label">Control Mode</div>
<select id="modeSelect" onchange="setMode(this.value)">
<option value="sensor">Sensor Mode</option>
<option value="mobile">Mobile Mode</option>
<option value="weather">Weather Mode</option>
</select>
<p class="small">Sensor Mode uses the physical rain sensor. Weather Mode uses weather data and the physical sensor. Mobile Mode enables manual control (the shield still folds once when rain starts).</p>
</section>

<section class="card">
<div class="label">Manual Control</div>
<div class="grid">
<button id="foldBtn" class="fold" onclick="sendMove('fold')">FOLD</button>
<button id="openBtn" class="open" onclick="sendMove('open')">OPEN</button>
</div>
<p class="small">Manual controls are enabled only in Mobile Mode.</p>
</section>

<section class="card">
<div class="label">Current Servo Position</div>
<div class="angle"><span id="angleValue">0</span><span class="degree">&deg;</span></div>
<div class="track"><div id="progressBar" class="progress"></div></div>
<p class="scale">0&deg; Fold &nbsp; | &nbsp; 80&deg; Open</p>
</section>

<section class="card">
<div class="label">Physical Sensor</div>
<div id="rainStatus" class="status-box">NO RAIN</div>
<div class="row"><span>Digital Output</span><span id="doValue">1</span></div>
</section>

<section class="card">
<div class="label">Current Weather</div>
<div id="weatherIcon" class="weather-icon">--</div>
<div id="weatherDescription" class="weather-name">Weather unavailable</div>
<div class="row"><span>Temperature</span><span id="temperature">--</span></div>
<div class="row"><span>Feels Like</span><span id="feelsLike">--</span></div>
<div class="row"><span>Humidity</span><span id="humidity">--</span></div>
<div class="row"><span>Cloud Cover</span><span id="cloudCover">--</span></div>
</section>

<section class="card">
<div class="label">Rain Information</div>
<div class="row"><span>Current Precipitation</span><span id="precipitation">--</span></div>
<div class="row"><span>Current Rain</span><span id="rainNow">--</span></div>
<div class="row"><span>Current Showers</span><span id="showers">--</span></div>
<div class="row"><span>Next 3h Probability</span><span id="probability">--</span></div>
<div class="prob"><div id="probBar"></div></div>
<div id="weatherDecision" class="decision">MONITORING WEATHER</div>
<div class="row"><span>Likely Rain Time</span><span id="rainTime">--</span></div>
</section>

<section class="card">
<div class="label">Wind</div>
<div class="row"><span>Speed</span><span id="windSpeed">--</span></div>
<div class="row"><span>Direction</span><span id="windDirection">--</span></div>
<div class="row"><span>Gusts</span><span id="windGust">--</span></div>
</section>

<section class="card">
<div class="label">Atmosphere</div>
<div class="row"><span>Pressure</span><span id="pressure">--</span></div>
<div class="row"><span>UV Index</span><span id="uv">--</span></div>
<div class="row"><span>Sunrise</span><span id="sunrise">--:--</span></div>
<div class="row"><span>Sunset</span><span id="sunset">--:--</span></div>
</section>

<section class="card">
<div class="label">Network</div>
<div class="row"><span>Home Wi-Fi</span><span id="internet">Disconnected</span></div>
<div class="row"><span>Weather API</span><span id="apiStatus">Disconnected</span></div>
<div class="row"><span>Weather Error</span><span id="weatherErr">--</span></div>
<div class="row"><span>Home Wi-Fi IP</span><span id="homeIP">--</span></div>
<div class="row"><span>RainShield IP</span><span id="apIP">192.168.4.1</span></div>
<div class="row"><span>Signal</span><span id="rssi">--</span></div>
<p class="note">Dashboard:<br>http://192.168.4.1</p>
<p class="note">Home Wi-Fi:<br>http://rainshield.local (or the Home Wi-Fi IP above)</p>
</section>

<section class="card">
<div class="label">Weather Location</div>
<label class="small" for="lat">Latitude</label>
<input id="lat" type="number" step="0.000001" inputmode="decimal">
<label class="small" for="lon">Longitude</label>
<input id="lon" type="number" step="0.000001" inputmode="decimal">
<button style="margin-top:12px" onclick="saveLocation()">SAVE LOCATION</button>
<p class="note">The location is saved inside the ESP32 and remains after restart.</p>
</section>

<section class="card">
<p class="note">Last weather update: <span id="lastUpdate">Never</span></p>
<p class="note">Weather information is refreshed automatically.</p>
<p class="note">Open-Meteo is used as the weather data source.</p>
</section>

<div id="connection" class="connection">Connected to RainShield</div>
<div class="footer">RainShield | ESP32 Smart Rain Protection System</div>
</main>

<script>
let busy=false;
let locationLoaded=false;
let posTimer=null;
function setAngleUI(a){
  const v=Math.max(0,Math.min(80,Number(a)||0));
  document.getElementById('angleValue').textContent=v.toFixed(0);
  document.getElementById('progressBar').style.width=(v/80*100)+'%';
}
// Poll the servo position quickly until the move has finished, then snap to the exact target.
function pollPos(){
  api('/pos').then(r=>r.json()).then(p=>{
    setAngleUI(p.angle);
    if(p.moving){
      posTimer=setTimeout(pollPos,120);
    }else{
      posTimer=null;
      setAngleUI(p.target);
      updateStatus();
    }
  }).catch(()=>{posTimer=null;});
}
function trackMove(){
  if(posTimer){clearTimeout(posTimer);posTimer=null;}
  pollPos();
}
function api(url){return fetch(url,{cache:'no-store'}).then(r=>{if(!r.ok)throw new Error('Request failed');return r;});}
function fmt(v,unit=''){return v===null||v===undefined||Number.isNaN(Number(v))?'--':Number(v).toFixed(1)+unit;}
function weatherIcon(code){
  if(code===null||code<0)return '--';
  if(code===0)return 'CLEAR';
  if(code<=3)return 'CLOUDY';
  if(code===45||code===48)return 'FOG';
  if(code>=51&&code<=57)return 'DRIZZLE';
  if(code>=61&&code<=67)return 'RAIN';
  if(code>=80&&code<=82)return 'SHOWERS';
  if(code>=95)return 'STORM';
  return 'WEATHER';
}
function updateStatus(){
  if(busy)return;
  api('/status').then(r=>r.json()).then(d=>{
    const folded=Number(d.target)===0;
    document.getElementById('systemStatus').textContent=d.moving?(folded?'FOLDING':'OPENING'):(folded?'FOLDED':'OPEN');
    if(d.mode==='SENSOR MODE'){
      document.getElementById('modeBadge').textContent='AUTO';
      document.getElementById('modeBadge').className='badge';
    }else if(d.mode==='MOBILE MODE'){
      document.getElementById('modeBadge').textContent='MANUAL';
      document.getElementById('modeBadge').className='badge mobile';
    }else{
      document.getElementById('modeBadge').textContent='WEATHER';
      document.getElementById('modeBadge').className='badge weather';
    }
    document.getElementById('modeSelect').value=d.mode==='SENSOR MODE'?'sensor':d.mode==='MOBILE MODE'?'mobile':'weather';
    document.getElementById('modeLine').innerHTML='Mode: '+d.mode.replace(' MODE','')+' | Servo: '+Number(d.angle).toFixed(0)+'&deg;';
    setAngleUI(d.angle);
    if(d.moving&&!posTimer)trackMove();   // e.g. the rain sensor started a move on its own
    document.getElementById('openBtn').disabled=d.mode!=='MOBILE MODE';
    document.getElementById('foldBtn').disabled=d.mode!=='MOBILE MODE';
    document.getElementById('rainStatus').textContent=d.rain;
    document.getElementById('rainStatus').className='status-box '+(d.rain==='RAIN DETECTED'?'rain':'');
    document.getElementById('doValue').textContent=d.do;

    document.getElementById('weatherIcon').textContent=weatherIcon(d.weatherCode);
    document.getElementById('weatherDescription').textContent=d.weatherDescription;
    document.getElementById('temperature').textContent=fmt(d.temperature,' °C');
    document.getElementById('feelsLike').textContent=fmt(d.feelsLike,' °C');
    document.getElementById('humidity').textContent=fmt(d.humidity,' %');
    document.getElementById('cloudCover').textContent=fmt(d.cloudCover,' %');
    document.getElementById('precipitation').textContent=fmt(d.precipitation,' mm');
    document.getElementById('rainNow').textContent=fmt(d.rainAmount,' mm');
    document.getElementById('showers').textContent=fmt(d.showers,' mm');
    document.getElementById('probability').textContent=d.probability<0?'--':d.probability+' %';
    document.getElementById('probBar').style.width=(Math.max(0,Math.min(100,d.probability))||0)+'%';
    document.getElementById('weatherDecision').textContent=d.weatherDecision;
    document.getElementById('rainTime').textContent=d.likelyRainTime;
    document.getElementById('windSpeed').textContent=fmt(d.windSpeed,' km/h');
    document.getElementById('windDirection').textContent=fmt(d.windDirection,' °');
    document.getElementById('windGust').textContent=fmt(d.windGust,' km/h');
    document.getElementById('pressure').textContent=fmt(d.pressure,' hPa');
    document.getElementById('uv').textContent=fmt(d.uv,'');
    document.getElementById('sunrise').textContent=d.sunrise;
    document.getElementById('sunset').textContent=d.sunset;
    document.getElementById('internet').textContent=d.internet?'CONNECTED':'DISCONNECTED';
    document.getElementById('apiStatus').textContent=d.weatherAvailable?'CONNECTED':'DISCONNECTED';
    document.getElementById('weatherErr').textContent=d.weatherError||'None';
    document.getElementById('homeIP').textContent=d.homeIP;
    document.getElementById('rssi').textContent=d.rssi;
    if(!locationLoaded){
      document.getElementById('lat').value=d.latitude;
      document.getElementById('lon').value=d.longitude;
      locationLoaded=true;
    }
    document.getElementById('lastUpdate').textContent=d.lastWeatherUpdate;
    const c=document.getElementById('connection');
    c.textContent='Connected to RainShield';
    c.classList.remove('offline');
  }).catch(()=>{
    const c=document.getElementById('connection');
    c.textContent='Connection lost - reconnect to RAIN SENSOR Wi-Fi';
    c.classList.add('offline');
  });
}
function setMode(mode){
  busy=true;
  api('/mode?value='+encodeURIComponent(mode)).then(()=>trackMove()).catch(()=>alert('Unable to change mode.')).finally(()=>{busy=false;});
}
function sendMove(command){
  if(document.getElementById('modeSelect').value!=='mobile')return;
  busy=true;
  api('/move?value='+encodeURIComponent(command)).then(()=>trackMove()).catch(()=>alert('Unable to control servo.')).finally(()=>{busy=false;});
}
function saveLocation(){
  const lat=document.getElementById('lat').value;
  const lon=document.getElementById('lon').value;
  if(!lat||!lon){alert('Enter both latitude and longitude.');return;}
  busy=true;
  api('/location?lat='+encodeURIComponent(lat)+'&lon='+encodeURIComponent(lon)).then(()=>alert('Location saved. Weather will refresh in a few seconds.')).catch(()=>alert('Unable to save location.')).finally(()=>{busy=false;updateStatus();});
}
updateStatus();
setInterval(updateStatus,2000);
</script>
</body>
</html>
)rawliteral";

void handleRoot() {
  server.send_P(200, "text/html; charset=utf-8", webpage);
}

void handleStatus() {
  String lastText;
  if (lastWeatherUpdate == 0) {
    lastText = "Never";
  } else {
    unsigned long secs = (millis() - lastWeatherUpdate) / 1000;
    if (secs < 60) lastText = String(secs) + " s ago";
    else lastText = String(secs / 60) + " min ago";
  }

  bool fresh = weatherFresh();
  String decision = fresh ? String("") : (weatherEverOk ? String("WEATHER DATA STALE") : String("MONITORING WEATHER"));

  lockData();   // weather data is written by another task

  if (fresh) decision = wx.decision;

  String json = "{";
  json += "\"mode\":\"" + jsonEscape(modeName()) + "\",";
  json += "\"rain\":\"" + String(rainDetected ? "RAIN DETECTED" : "NO RAIN") + "\",";
  json += "\"do\":" + String(currentDO) + ",";
  json += "\"angle\":" + String(currentAngle, 1) + ",";
  json += "\"target\":" + String((int)roundf(targetAngle)) + ",";
  json += "\"moving\":" + String(servoMoving ? "true" : "false") + ",";
  json += "\"weatherAvailable\":" + String(fresh ? "true" : "false") + ",";
  json += "\"weatherError\":\"" + jsonEscape(weatherError) + "\",";
  json += "\"weatherCode\":" + String(wx.code) + ",";
  json += "\"weatherDescription\":\"" + jsonEscape(wx.description) + "\",";
  json += "\"temperature\":" + numberOrNull(wx.temperature) + ",";
  json += "\"feelsLike\":" + numberOrNull(wx.feelsLike) + ",";
  json += "\"humidity\":" + numberOrNull(wx.humidity) + ",";
  json += "\"cloudCover\":" + numberOrNull(wx.cloudCover) + ",";
  json += "\"precipitation\":" + numberOrNull(wx.precipitation, 2) + ",";
  json += "\"rainAmount\":" + numberOrNull(wx.rain, 2) + ",";
  json += "\"showers\":" + numberOrNull(wx.showers, 2) + ",";
  json += "\"probability\":" + String(wx.next3hProbability) + ",";
  json += "\"windSpeed\":" + numberOrNull(wx.windSpeed) + ",";
  json += "\"windDirection\":" + numberOrNull(wx.windDirection) + ",";
  json += "\"windGust\":" + numberOrNull(wx.windGust) + ",";
  json += "\"pressure\":" + numberOrNull(wx.pressure) + ",";
  json += "\"uv\":" + numberOrNull(wx.uv) + ",";
  json += "\"sunrise\":\"" + jsonEscape(wx.sunrise) + "\",";
  json += "\"sunset\":\"" + jsonEscape(wx.sunset) + "\",";
  json += "\"weatherDecision\":\"" + jsonEscape(decision) + "\",";
  json += "\"likelyRainTime\":\"" + jsonEscape(wx.likelyRainTime) + "\",";
  json += "\"latitude\":" + String(weatherLatitude, 6) + ",";
  json += "\"longitude\":" + String(weatherLongitude, 6) + ",";

  unlockData();

  json += "\"internet\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false") + ",";
  json += "\"homeIP\":\"" + WiFi.localIP().toString() + "\",";
  json += "\"rssi\":" + String(WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0) + ",";
  json += "\"lastWeatherUpdate\":\"" + jsonEscape(lastText) + "\"";
  json += "}";

  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json; charset=utf-8", json);
}

// Tiny endpoint the dashboard polls every ~120 ms while the servo is moving.
void handlePos() {
  String json = "{\"angle\":" + String(currentAngle, 1) +
                ",\"target\":" + String((int)roundf(targetAngle)) +
                ",\"moving\":" + String(servoMoving ? "true" : "false") + "}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json; charset=utf-8", json);
}

void handleMode() {
  if (!server.hasArg("value")) {
    server.send(400, "text/plain; charset=utf-8", "Missing mode");
    return;
  }

  String mode = server.arg("value");

  if (mode == "sensor") {
    controlMode = SENSOR_MODE;
    applyAutomaticDecision();
    server.send(200, "text/plain; charset=utf-8", "SENSOR MODE");
  } else if (mode == "mobile") {
    controlMode = MOBILE_MODE;
    server.send(200, "text/plain; charset=utf-8", "MOBILE MODE");
  } else if (mode == "weather") {
    controlMode = WEATHER_MODE;
    applyAutomaticDecision();
    server.send(200, "text/plain; charset=utf-8", "WEATHER MODE");
  } else {
    server.send(400, "text/plain; charset=utf-8", "Invalid mode");
  }
}

void handleMove() {
  if (controlMode != MOBILE_MODE) {
    server.send(403, "text/plain; charset=utf-8", "Manual control is available only in Mobile Mode");
    return;
  }

  if (!server.hasArg("value")) {
    server.send(400, "text/plain; charset=utf-8", "Missing command");
    return;
  }

  String command = server.arg("value");
  if (command == "open") {
    moveServo(OPEN_ANGLE);
    server.send(200, "text/plain; charset=utf-8", "OPEN");
  } else if (command == "fold") {
    moveServo(FOLD_ANGLE);
    server.send(200, "text/plain; charset=utf-8", "FOLD");
  } else {
    server.send(400, "text/plain; charset=utf-8", "Invalid command");
  }
}

void handleLocation() {
  if (!server.hasArg("lat") || !server.hasArg("lon")) {
    server.send(400, "text/plain; charset=utf-8", "Missing latitude or longitude");
    return;
  }

  double lat = server.arg("lat").toDouble();
  double lon = server.arg("lon").toDouble();

  if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) {
    server.send(400, "text/plain; charset=utf-8", "Invalid coordinates");
    return;
  }

  lockData();
  weatherLatitude = lat;
  weatherLongitude = lon;
  unlockData();

  preferences.putDouble("latitude", lat);
  preferences.putDouble("longitude", lon);

  // Old data belongs to the old location, so stop trusting it
  // and ask the weather task to fetch right away.
  weatherEverOk = false;
  forceWeatherFetch = true;

  server.send(200, "text/plain; charset=utf-8", "Location saved. Weather is refreshing.");
}

// =====================================================
// SETUP
// =====================================================

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.print("Reset reason: ");
  Serial.println((int)esp_reset_reason());   // 1 power-on, 4 panic, 9 brownout

  dataMutex = xSemaphoreCreateMutex();

  pinMode(RAIN_SENSOR_PIN, INPUT);

  ESP32PWM::allocateTimer(0);
  servoAttachIfNeeded();
  myServo.write(OPEN_ANGLE);
  currentAngle = startAngle = targetAngle = OPEN_ANGLE;
  lastServoActivity = millis();

  preferences.begin("rainshield", false);
  weatherLatitude = preferences.getDouble("latitude", DEFAULT_LATITUDE);
  weatherLongitude = preferences.getDouble("longitude", DEFAULT_LONGITUDE);

  currentDO = digitalRead(RAIN_SENSOR_PIN);
  lastRawState = currentDO;
  rainDetected = (currentDO == RAIN_DETECTED);
  stableCount = REQUIRED_STABLE_READINGS;

  WiFi.mode(WIFI_AP_STA);
  WiFi.setAutoReconnect(true);
  WiFi.onEvent(onWiFiEvent);
  delay(500);

  startAccessPoint();
  delay(500);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/pos", HTTP_GET, handlePos);
  server.on("/mode", HTTP_GET, handleMode);
  server.on("/move", HTTP_GET, handleMove);
  server.on("/location", HTTP_GET, handleLocation);

  // Direct browser shortcuts: /fold and /open
  server.on("/fold", HTTP_GET, []() {
    if (controlMode != MOBILE_MODE) {
      server.send(403, "text/plain; charset=utf-8", "Switch to Mobile Mode before using /fold");
      return;
    }
    moveServo(FOLD_ANGLE);
    server.send(200, "text/plain; charset=utf-8", "FOLD command sent");
  });

  server.on("/open", HTTP_GET, []() {
    if (controlMode != MOBILE_MODE) {
      server.send(403, "text/plain; charset=utf-8", "Switch to Mobile Mode before using /open");
      return;
    }
    moveServo(OPEN_ANGLE);
    server.send(200, "text/plain; charset=utf-8", "OPEN command sent");
  });

  server.begin();

  // Start home Wi-Fi connection only after AP and server are ready.
  // mDNS is started from loop() once the station gets an IP.
  connectHomeWiFi();

  // Weather runs in its own task on core 0 so it can never block the servo.
  xTaskCreatePinnedToCore(weatherTask, "weather", 16384, NULL, 1, NULL, 0);

  Serial.println();
  Serial.println("==============================");
  Serial.println("RAINSHIELD STARTED");
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());
  Serial.println("==============================");

  applyAutomaticDecision();
}

// =====================================================
// LOOP
// =====================================================

void loop() {
  server.handleClient();
  updateRainSensor();
  updateServoMovement();

  // New weather arrived from the weather task: apply the servo decision here,
  // on the main core, so only one place ever controls the servo.
  // Also re-check when weather goes stale or comes back.
  static bool wasFresh = false;
  bool nowFresh = weatherFresh();
  if (weatherDirty || nowFresh != wasFresh) {
    weatherDirty = false;
    wasFresh = nowFresh;
    if (controlMode == WEATHER_MODE) applyAutomaticDecision();
  }

  // (Re)start mDNS after the home Wi-Fi obtains an IP.
  if (mdnsNeedsStart) {
    mdnsNeedsStart = false;
    MDNS.end();
    if (MDNS.begin("rainshield")) {
      MDNS.addService("http", "tcp", 80);
      Serial.println("mDNS started: http://rainshield.local");
    }
  }

  // Retry the hotspot in the background. No WiFi.disconnect() here:
  // it would interrupt the RAIN SENSOR access point.
  if (homeWiFiConfigured() && WiFi.status() != WL_CONNECTED &&
      millis() - lastWiFiReconnectAttempt >= WIFI_RECONNECT_INTERVAL) {
    lastWiFiReconnectAttempt = millis();
    Serial.println("Wi-Fi not connected, retrying...");
    WiFi.begin(HOME_WIFI_SSID, HOME_WIFI_PASSWORD);
  }

  // Status report on Serial every 10 s
  if (millis() - lastSerialReport >= 10000) {
    lastSerialReport = millis();
    Serial.print("[Status] WiFi=");
    Serial.print((int)WiFi.status());
    Serial.print(" IP=");
    Serial.print(WiFi.localIP());
    Serial.print(" weatherFresh=");
    Serial.print(weatherFresh() ? "yes" : "no");
    Serial.print(" servo=");
    Serial.print(currentAngle, 0);
    Serial.print(servoAttached ? " (attached)" : " (detached)");
    Serial.print(" heap=");
    Serial.println(ESP.getFreeHeap());
  }

  delay(1);
}
