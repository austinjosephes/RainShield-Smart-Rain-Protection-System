#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <ESP32Servo.h>
#include <math.h>

// =====================================================
// SMART RAIN SHIELD
// ESP32 + DIGITAL RAIN SENSOR + MG90S + OPEN-METEO
// =====================================================

// ---------------- PIN DEFINITIONS ---------------------
#define RAIN_SENSOR_PIN 27
#define SERVO_PIN       13

// ---------------- RAIN SENSOR LOGIC -------------------
// HIGH = DRY
// LOW  = RAIN / WET
#define RAIN_DETECTED LOW

// ---------------- SERVO SETTINGS ----------------------
const int OPEN_ANGLE = 90;
const int FOLD_ANGLE = 0;
const unsigned long FULL_MOVE_TIME = 1500;
const int SERVO_MIN_US = 500;
const int SERVO_MAX_US = 2400;

// ---------------- ESP32 ACCESS POINT ------------------
const char* AP_SSID = "RainShield";
const char* AP_PASSWORD = "12345678";
IPAddress AP_IP(192, 168, 4, 1);
IPAddress AP_GATEWAY(192, 168, 4, 1);
IPAddress AP_SUBNET(255, 255, 255, 0);

// ---------------- HOME WI-FI --------------------------
const char* HOME_WIFI_SSID = "Keralavision4796";
// IMPORTANT: put your actual home Wi-Fi password here.
const char* HOME_WIFI_PASSWORD = "password@123";

// ---------------- DEFAULT WEATHER LOCATION -----------
const double DEFAULT_LATITUDE = 10.607028;
const double DEFAULT_LONGITUDE = 76.199542;

// ---------------- WEATHER SETTINGS --------------------
const unsigned long WEATHER_UPDATE_INTERVAL = 10UL * 60UL * 1000UL;
const unsigned long WIFI_CONNECT_TIMEOUT = 20000;
const unsigned long WEATHER_HTTP_TIMEOUT = 3000;
const unsigned long WIFI_RECONNECT_INTERVAL = 10000;
unsigned long lastWiFiReconnectAttempt = 0;

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
bool servoMoving = false;

// ---------------- SENSOR STATE ------------------------
bool rainDetected = false;
int currentDO = HIGH;
int lastRawState = HIGH;
int stableCount = 0;
unsigned long lastSensorCheck = 0;
const unsigned long SENSOR_INTERVAL = 50;
const int REQUIRED_STABLE_READINGS = 5;

// ---------------- WEATHER STATE -----------------------
double weatherLatitude = DEFAULT_LATITUDE;
double weatherLongitude = DEFAULT_LONGITUDE;

bool weatherAvailable = false;
bool weatherFetching = false;
unsigned long lastWeatherUpdate = 0;
String weatherError = "";

float weatherTemperature = NAN;
float weatherFeelsLike = NAN;
float weatherHumidity = NAN;
float weatherCloudCover = NAN;
float weatherPrecipitation = NAN;
float weatherRain = NAN;
float weatherShowers = NAN;
float weatherWindSpeed = NAN;
float weatherWindDirection = NAN;
float weatherWindGust = NAN;
float weatherPressure = NAN;
float weatherUV = NAN;
int weatherCode = -1;
int next3hProbability = -1;
float next3hPrecipitation = NAN;
float next3hRain = NAN;
float next3hShowers = NAN;
String likelyRainTime = "No significant rain forecast";
String weatherDecision = "MONITORING WEATHER";
String weatherDescription = "Weather unavailable";
String sunriseTime = "--:--";
String sunsetTime = "--:--";
String lastWeatherUpdateText = "Never";

// =====================================================
// HELPER FUNCTIONS
// =====================================================

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
  if (t >= 0 && iso.length() >= t + 6) {
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

void updateWeatherDecision() {
  bool rainNow = false;
  bool rainLikely = false;

  if (!isnan(weatherPrecipitation) && weatherPrecipitation > 0.1f) rainNow = true;
  if (!isnan(weatherRain) && weatherRain > 0.1f) rainNow = true;
  if (!isnan(weatherShowers) && weatherShowers > 0.1f) rainNow = true;
  if (codeMeansRain(weatherCode)) rainNow = true;

  if (next3hProbability >= 60) rainLikely = true;
  if (!isnan(next3hRain) && next3hRain >= 0.2f) rainLikely = true;
  if (!isnan(next3hShowers) && next3hShowers >= 0.2f) rainLikely = true;
  if (!isnan(next3hPrecipitation) && next3hPrecipitation >= 0.2f) rainLikely = true;

  if (rainNow) weatherDecision = "RAIN NOW";
  else if (rainLikely) weatherDecision = "RAIN LIKELY";
  else if (weatherAvailable) weatherDecision = "WEATHER LOOKS SAFE";
  else weatherDecision = "MONITORING WEATHER";
}

// =====================================================
// SERVO MOVEMENT
// =====================================================

void updateServoMovement() {
  if (!servoMoving) return;

  unsigned long elapsed = millis() - moveStartTime;

  if (moveDuration == 0 || elapsed >= moveDuration) {
    currentAngle = targetAngle;
    myServo.write((int)roundf(currentAngle));
    servoMoving = false;
    return;
  }

  float progress = (float)elapsed / (float)moveDuration;
  float smoothProgress = progress * progress * (3.0f - 2.0f * progress);
  currentAngle = startAngle + ((targetAngle - startAngle) * smoothProgress);
  myServo.write((int)roundf(currentAngle));
}

void moveServo(int newAngle) {
  newAngle = constrain(newAngle, FOLD_ANGLE, OPEN_ANGLE);
  updateServoMovement();

  if (!servoMoving && newAngle == (int)roundf(currentAngle)) return;
  if (servoMoving && newAngle == (int)roundf(targetAngle)) return;

  startAngle = currentAngle;
  targetAngle = (float)newAngle;
  moveStartTime = millis();

  float distance = fabsf(targetAngle - startAngle);
  moveDuration = (unsigned long)(FULL_MOVE_TIME * distance / 90.0f);
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
    } else if (weatherAvailable) {
      bool weatherRisk = (weatherDecision == "RAIN NOW" ||
                          weatherDecision == "RAIN LIKELY");
      moveServo(weatherRisk ? FOLD_ANGLE : OPEN_ANGLE);
    }
    // If weather is unavailable, hold the current position.
  }
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
  }

  if (stableCount >= REQUIRED_STABLE_READINGS) {
    bool detected = (rawState == RAIN_DETECTED);
    if (detected != rainDetected) {
      rainDetected = detected;
      applyAutomaticDecision();
    }
  }
}

// =====================================================
// WEATHER API
// =====================================================

void clearWeatherData() {
  weatherAvailable = false;
  weatherTemperature = NAN;
  weatherFeelsLike = NAN;
  weatherHumidity = NAN;
  weatherCloudCover = NAN;
  weatherPrecipitation = NAN;
  weatherRain = NAN;
  weatherShowers = NAN;
  weatherWindSpeed = NAN;
  weatherWindDirection = NAN;
  weatherWindGust = NAN;
  weatherPressure = NAN;
  weatherUV = NAN;
  weatherCode = -1;
  next3hProbability = -1;
  next3hPrecipitation = NAN;
  next3hRain = NAN;
  next3hShowers = NAN;
  likelyRainTime = "No significant rain forecast";
  weatherDecision = "MONITORING WEATHER";
  weatherDescription = "Weather unavailable";
  sunriseTime = "--:--";
  sunsetTime = "--:--";
}

bool fetchWeather() {
  if (WiFi.status() != WL_CONNECTED) {
    weatherError = "Home Wi-Fi is not connected";
    clearWeatherData();
    return false;
  }

  weatherFetching = true;

  String url = "https://api.open-meteo.com/v1/forecast?latitude=";
  url += String(weatherLatitude, 6);
  url += "&longitude=";
  url += String(weatherLongitude, 6);
  url += "&current=temperature_2m,relative_humidity_2m,apparent_temperature,is_day,precipitation,rain,showers,weather_code,cloud_cover,pressure_msl,wind_speed_10m,wind_direction_10m,wind_gusts_10m";
  url += "&hourly=precipitation_probability,precipitation,rain,showers,weather_code";
  url += "&daily=uv_index_max,precipitation_probability_max,precipitation_sum,rain_sum,showers_sum,sunrise,sunset";
  url += "&forecast_hours=6&forecast_days=1&timezone=auto";

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(WEATHER_HTTP_TIMEOUT);

  if (!http.begin(client, url)) {
    weatherError = "Could not start weather request";
    weatherFetching = false;
    clearWeatherData();
    return false;
  }

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    weatherError = "Weather API HTTP error: " + String(httpCode);
    http.end();
    weatherFetching = false;
    clearWeatherData();
    return false;
  }

  String payload = http.getString();
  http.end();

  DynamicJsonDocument doc(45000);
  DeserializationError error = deserializeJson(doc, payload);

  if (error) {
    weatherError = "Weather JSON parsing failed";
    weatherFetching = false;
    clearWeatherData();
    return false;
  }

  JsonObject current = doc["current"];
  if (current.isNull()) {
    weatherError = "Weather response has no current data";
    weatherFetching = false;
    clearWeatherData();
    return false;
  }

  weatherTemperature = current["temperature_2m"] | NAN;
  weatherFeelsLike = current["apparent_temperature"] | NAN;
  weatherHumidity = current["relative_humidity_2m"] | NAN;
  weatherCloudCover = current["cloud_cover"] | NAN;
  weatherPrecipitation = current["precipitation"] | NAN;
  weatherRain = current["rain"] | NAN;
  weatherShowers = current["showers"] | NAN;
  weatherWindSpeed = current["wind_speed_10m"] | NAN;
  weatherWindDirection = current["wind_direction_10m"] | NAN;
  weatherWindGust = current["wind_gusts_10m"] | NAN;
  weatherPressure = current["pressure_msl"] | NAN;
  weatherCode = current["weather_code"] | -1;
  weatherDescription = weatherTextFromCode(weatherCode);

  JsonObject daily = doc["daily"];
  if (!daily.isNull()) {
    weatherUV = daily["uv_index_max"][0] | NAN;
    const char* sunrise = daily["sunrise"][0] | "";
    const char* sunset = daily["sunset"][0] | "";
    sunriseTime = timeOnly(String(sunrise));
    sunsetTime = timeOnly(String(sunset));
  }

  next3hProbability = -1;
  next3hPrecipitation = 0.0f;
  next3hRain = 0.0f;
  next3hShowers = 0.0f;
  likelyRainTime = "No significant rain forecast";

  JsonArray hourlyTimes = doc["hourly"]["time"];
  JsonArray hourlyProb = doc["hourly"]["precipitation_probability"];
  JsonArray hourlyPrec = doc["hourly"]["precipitation"];
  JsonArray hourlyRain = doc["hourly"]["rain"];
  JsonArray hourlyShowers = doc["hourly"]["showers"];

  int count = hourlyTimes.size();
  int maxProbability = 0;
  bool foundRainTime = false;

  // The API's first hourly entry corresponds to the current hour.
  // Inspect the next three hours.
  for (int i = 0; i < count && i < 4; i++) {
    int probability = hourlyProb[i] | 0;
    float precipitation = hourlyPrec[i] | 0.0f;
    float rain = hourlyRain[i] | 0.0f;
    float showers = hourlyShowers[i] | 0.0f;

    if (probability > maxProbability) maxProbability = probability;
    next3hPrecipitation += precipitation;
    next3hRain += rain;
    next3hShowers += showers;

    if (!foundRainTime && probability >= 60) {
      const char* t = hourlyTimes[i] | "";
      likelyRainTime = String(t);
      foundRainTime = true;
    }
  }

  next3hProbability = maxProbability;

  weatherAvailable = true;
  weatherError = "";
  lastWeatherUpdate = millis();
  lastWeatherUpdateText = String(millis() / 1000) + " seconds ago";

  updateWeatherDecision();
  applyAutomaticDecision();

  weatherFetching = false;
  return true;
}

// =====================================================
// WIFI
// =====================================================

void startAccessPoint() {
  WiFi.softAPConfig(AP_IP, AP_GATEWAY, AP_SUBNET);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
}

void connectHomeWiFi() {
  if (String(HOME_WIFI_PASSWORD) == "YOUR_WIFI_PASSWORD") {
    Serial.println("Home Wi-Fi password is not configured.");
    return;
  }

  WiFi.begin(HOME_WIFI_SSID, HOME_WIFI_PASSWORD);
  Serial.print("Connecting to home Wi-Fi");

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Home Wi-Fi IP: ");
    Serial.println(WiFi.localIP());

    if (MDNS.begin("rainshield")) {
      Serial.println("mDNS: http://rainshield.local");
    }

    fetchWeather();
  } else {
    Serial.println("Home Wi-Fi connection failed.");
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
.progress{height:100%;width:0;background:#1597ff;border-radius:999px;transition:width .15s}
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
<p class="small">Sensor Mode uses the physical rain sensor. Weather Mode uses weather data and the physical sensor. Mobile Mode enables manual control.</p>
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
<p class="scale">0&deg; Fold &nbsp; | &nbsp; 90&deg; Open</p>
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
<div class="row"><span>Internet</span><span id="internet">Disconnected</span></div>
<div class="row"><span>Weather API</span><span id="apiStatus">Disconnected</span></div>
<div class="row"><span>Home Wi-Fi IP</span><span id="homeIP">--</span></div>
<div class="row"><span>RainShield IP</span><span id="apIP">192.168.4.1</span></div>
<div class="row"><span>Signal</span><span id="rssi">--</span></div>
<p class="note">Local access:<br>http://192.168.4.1</p>
<p class="note">Home Wi-Fi:<br>http://rainshield.local</p>
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
    document.getElementById('systemStatus').textContent=d.rain==='RAIN DETECTED'?'SENSOR: FOLD':'SENSOR: OPEN';
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
    document.getElementById('angleValue').textContent=Number(d.angle).toFixed(0);
    document.getElementById('progressBar').style.width=(Math.max(0,Math.min(90,Number(d.angle)))/90*100)+'%';
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
    document.getElementById('homeIP').textContent=d.homeIP;
    document.getElementById('rssi').textContent=d.rssi;
    document.getElementById('lat').value=d.latitude;
    document.getElementById('lon').value=d.longitude;
    document.getElementById('lastUpdate').textContent=d.lastWeatherUpdate;
    const c=document.getElementById('connection');
    c.textContent='Connected to RainShield';
    c.classList.remove('offline');
  }).catch(()=>{
    const c=document.getElementById('connection');
    c.textContent='Connection lost - reconnect to RainShield Wi-Fi';
    c.classList.add('offline');
  });
}
function setMode(mode){
  busy=true;
  api('/mode?value='+encodeURIComponent(mode)).then(()=>updateStatus()).catch(()=>alert('Unable to change mode.')).finally(()=>{busy=false;updateStatus();});
}
function sendMove(command){
  if(document.getElementById('modeSelect').value!=='mobile')return;
  busy=true;
  api('/move?value='+encodeURIComponent(command)).then(()=>updateStatus()).catch(()=>alert('Unable to control servo.')).finally(()=>{busy=false;updateStatus();});
}
function saveLocation(){
  const lat=document.getElementById('lat').value;
  const lon=document.getElementById('lon').value;
  if(!lat||!lon){alert('Enter both latitude and longitude.');return;}
  busy=true;
  api('/location?lat='+encodeURIComponent(lat)+'&lon='+encodeURIComponent(lon)).then(()=>alert('Location saved and weather refreshed.')).catch(()=>alert('Unable to save location.')).finally(()=>{busy=false;updateStatus();});
}
updateStatus();
setInterval(updateStatus,5000);
</script>
</body>
</html>
)rawliteral";

void handleRoot(){
  server.send_P(200,"text/html; charset=utf-8",webpage);
}

void handleStatus(){
  String json="{";
  json+="\"mode\":\""+jsonEscape(modeName())+"\",";
  json+="\"rain\":\""+(rainDetected?String("RAIN DETECTED"):String("NO RAIN"))+"\",";
  json+="\"do\":"+String(currentDO)+",";
  json+="\"angle\":"+String(currentAngle,1)+",";
  json+="\"weatherAvailable\":"+(weatherAvailable?String("true"):String("false"))+",";
  json+="\"weatherCode\":"+String(weatherCode)+",";
  json+="\"weatherDescription\":\""+jsonEscape(weatherDescription)+"\",";
  json+="\"temperature\":"+numberOrNull(weatherTemperature)+",";
  json+="\"feelsLike\":"+numberOrNull(weatherFeelsLike)+",";
  json+="\"humidity\":"+numberOrNull(weatherHumidity)+",";
  json+="\"cloudCover\":"+numberOrNull(weatherCloudCover)+",";
  json+="\"precipitation\":"+numberOrNull(weatherPrecipitation,2)+",";
  json+="\"rainAmount\":"+numberOrNull(weatherRain,2)+",";
  json+="\"showers\":"+numberOrNull(weatherShowers,2)+",";
  json+="\"probability\":"+String(next3hProbability)+",";
  json+="\"windSpeed\":"+numberOrNull(weatherWindSpeed)+",";
  json+="\"windDirection\":"+numberOrNull(weatherWindDirection)+",";
  json+="\"windGust\":"+numberOrNull(weatherWindGust)+",";
  json+="\"pressure\":"+numberOrNull(weatherPressure)+",";
  json+="\"uv\":"+numberOrNull(weatherUV)+",";
  json+="\"sunrise\":\""+jsonEscape(sunriseTime)+"\",";
  json+="\"sunset\":\""+jsonEscape(sunsetTime)+"\",";
  json+="\"weatherDecision\":\""+jsonEscape(weatherDecision)+"\",";
  json+="\"likelyRainTime\":\""+jsonEscape(likelyRainTime)+"\",";
  json+="\"latitude\":"+String(weatherLatitude,6)+",";
  json+="\"longitude\":"+String(weatherLongitude,6)+",";
  json+="\"internet\":"+(WiFi.status()==WL_CONNECTED?String("true"):String("false"))+",";
  json+="\"homeIP\":\""+WiFi.localIP().toString()+"\",";
  json+="\"rssi\":"+String(WiFi.status()==WL_CONNECTED?WiFi.RSSI():0)+",";
  json+="\"lastWeatherUpdate\":\""+jsonEscape(lastWeatherUpdateText)+"\"";
  json+="}";

  server.sendHeader("Cache-Control","no-store");
  server.send(200,"application/json; charset=utf-8",json);
}

void handleMode(){
  if(!server.hasArg("value")){
    server.send(400,"text/plain; charset=utf-8","Missing mode");
    return;
  }

  String mode=server.arg("value");

  if(mode=="sensor"){
    controlMode=SENSOR_MODE;
    applyAutomaticDecision();
    server.send(200,"text/plain; charset=utf-8","SENSOR MODE");
  }else if(mode=="mobile"){
    controlMode=MOBILE_MODE;
    server.send(200,"text/plain; charset=utf-8","MOBILE MODE");
  }else if(mode=="weather"){
    controlMode=WEATHER_MODE;
    applyAutomaticDecision();
    server.send(200,"text/plain; charset=utf-8","WEATHER MODE");
  }else{
    server.send(400,"text/plain; charset=utf-8","Invalid mode");
  }
}

void handleMove(){
  if(controlMode!=MOBILE_MODE){
    server.send(403,"text/plain; charset=utf-8","Manual control is available only in Mobile Mode");
    return;
  }

  if(!server.hasArg("value")){
    server.send(400,"text/plain; charset=utf-8","Missing command");
    return;
  }

  String command=server.arg("value");
  if(command=="open"){
    moveServo(OPEN_ANGLE);
    server.send(200,"text/plain; charset=utf-8","OPEN");
  }else if(command=="fold"){
    moveServo(FOLD_ANGLE);
    server.send(200,"text/plain; charset=utf-8","FOLD");
  }else{
    server.send(400,"text/plain; charset=utf-8","Invalid command");
  }
}

void handleLocation(){
  if(!server.hasArg("lat") || !server.hasArg("lon")){
    server.send(400,"text/plain; charset=utf-8","Missing latitude or longitude");
    return;
  }

  double lat=server.arg("lat").toDouble();
  double lon=server.arg("lon").toDouble();

  if(lat<-90.0 || lat>90.0 || lon<-180.0 || lon>180.0){
    server.send(400,"text/plain; charset=utf-8","Invalid coordinates");
    return;
  }

  weatherLatitude=lat;
  weatherLongitude=lon;
  preferences.putDouble("latitude",weatherLatitude);
  preferences.putDouble("longitude",weatherLongitude);

  bool ok=fetchWeather();
  server.send(ok?200:503,"text/plain; charset=utf-8",ok?"Location saved":"Location saved, but weather update failed");
}

// =====================================================
// SETUP
// =====================================================

void setup(){
  Serial.begin(115200);
  delay(200);

  pinMode(RAIN_SENSOR_PIN,INPUT);

  ESP32PWM::allocateTimer(0);
  myServo.setPeriodHertz(50);
  myServo.attach(SERVO_PIN,SERVO_MIN_US,SERVO_MAX_US);
  myServo.write(OPEN_ANGLE);
  currentAngle=startAngle=targetAngle=OPEN_ANGLE;

  preferences.begin("rainshield",false);
  weatherLatitude=preferences.getDouble("latitude",DEFAULT_LATITUDE);
  weatherLongitude=preferences.getDouble("longitude",DEFAULT_LONGITUDE);

  currentDO=digitalRead(RAIN_SENSOR_PIN);
  lastRawState=currentDO;
  rainDetected=(currentDO==RAIN_DETECTED);
  stableCount=REQUIRED_STABLE_READINGS;

  WiFi.mode(WIFI_AP_STA);
  startAccessPoint();
  connectHomeWiFi();

  server.on("/",HTTP_GET,handleRoot);
  server.on("/status",HTTP_GET,handleStatus);
  server.on("/mode",HTTP_GET,handleMode);
  server.on("/move",HTTP_GET,handleMove);
  server.on("/location",HTTP_GET,handleLocation);
  server.begin();

  Serial.println();
  Serial.println("==============================");
  Serial.println("RAINSHIELD STARTED");
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());
  Serial.print("Home Wi-Fi IP: ");
  Serial.println(WiFi.localIP());
  Serial.println("==============================");

  applyAutomaticDecision();
}

// =====================================================
// LOOP
// =====================================================

void loop(){
  server.handleClient();
  updateRainSensor();
  updateServoMovement();

  // Automatically reconnect the home Wi-Fi without stopping the RainShield AP.
  if(WiFi.status()!=WL_CONNECTED){
    if(millis()-lastWiFiReconnectAttempt>=WIFI_RECONNECT_INTERVAL){
      lastWiFiReconnectAttempt=millis();
      WiFi.disconnect();
      WiFi.begin(HOME_WIFI_SSID,HOME_WIFI_PASSWORD);
    }
  }

  // Refresh weather only while the servo is idle.
  // This prevents a weather HTTP request from interrupting servo movement.
  if(WiFi.status()==WL_CONNECTED && !servoMoving){
    if(lastWeatherUpdate==0 || millis()-lastWeatherUpdate>=WEATHER_UPDATE_INTERVAL){
      fetchWeather();
    }
  }
}
