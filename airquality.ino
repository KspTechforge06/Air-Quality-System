/*
 * ============================================================
 *  Air Quality Monitor — ESP8266 + MQ135 + SSD1306 + Buzzer
 * ============================================================
 *  Wiring:
 *    MQ135  AOUT → A0          (analog in)
 *    OLED   SDA  → D2 (GPIO4)  (I2C)
 *    OLED   SCL  → D1 (GPIO5)  (I2C)
 *    Buzzer +    → D5 (GPIO14) (active buzzer)
 *
 *  Libraries needed (install via Arduino Library Manager):
 *    - ESP8266WiFi        (built-in with ESP8266 board package)
 *    - ESP8266WebServer   (built-in)
 *    - Adafruit_SSD1306
 *    - Adafruit_GFX
 *    - ArduinoJson        (v6.x)
 * ============================================================
 */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>

// ── Wi-Fi credentials ──────────────────────────────────────
const char* WIFI_SSID     = "ksp's network";      // <-- change this
const char* WIFI_PASSWORD = "Prajwal.k";  // <-- change this

// ── Hardware pins ──────────────────────────────────────────
#define MQ135_PIN   A0
#define BUZZER_PIN  14  // GPIO14 d5

// ── OLED (128x64, I2C address 0x3C) ───────────────────────
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT  64
#define OLED_RESET     -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ── Web server on port 80 ──────────────────────────────────
ESP8266WebServer server(80);

// ── MQ135 calibration ─────────────────────────────────────
#define MQ135_RZERO   76.63
#define MQ135_RLOAD   10.0
#define VCC           1.0
#define ADC_MAX       1023.0

// ── Global state ───────────────────────────────────────────
float         g_aqiValue    = 0;
String        g_aqiCategory = "Good";
bool          g_buzzerFired = false;
unsigned long g_lastRead    = 0;
const unsigned long READ_INTERVAL = 2000;

// ── Float version of map() ─────────────────────────────────
float mapf(float x, float in_min, float in_max, float out_min, float out_max) {
  return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

// ── Convert raw ADC to AQI ─────────────────────────────────
float rawToAQI(int raw) {
  Serial.print("ADC raw = "); Serial.println(raw); // debug

  float vout  = (raw / 1023.0) * 1.0;   // 1.0V ref for ESP8266
  if (vout <= 0) vout = 0.01;            // avoid divide-by-zero
  float rs    = ((1.0 - vout) / vout) * MQ135_RLOAD;
  float ratio = rs / MQ135_RZERO;
  float ppm   = 116.6020682 * pow(ratio, -2.769034857);

  Serial.print("PPM = "); Serial.println(ppm); // debug

  float aqi;
  if      (ppm < 400)  aqi = mapf(ppm,    0,  400,   0,  50);
  else if (ppm < 700)  aqi = mapf(ppm,  400,  700,  50, 100);
  else if (ppm < 1000) aqi = mapf(ppm,  700, 1000, 100, 150);
  else if (ppm < 2000) aqi = mapf(ppm, 1000, 2000, 150, 200);
  else if (ppm < 5000) aqi = mapf(ppm, 2000, 5000, 200, 300);
  else                 aqi = 301;

  return constrain(aqi, 0, 500);
}

// ── AQI category string ────────────────────────────────────
String aqiCategory(float aqi) {
  if (aqi <= 50)  return "Good";
  if (aqi <= 100) return "Moderate";
  if (aqi <= 150) return "Sensitive";
  if (aqi <= 200) return "Unhealthy";
  if (aqi <= 300) return "Very Unhealthy";
  return "Hazardous";
}

// ── Update OLED ────────────────────────────────────────────
void updateDisplay(int raw) {
  display.clearDisplay();

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print("AIR QUALITY MONITOR");
  display.drawLine(0, 9, 127, 9, SSD1306_WHITE);

  display.setTextSize(3);
  display.setCursor(10, 16);
  display.print((int)g_aqiValue);

  display.setTextSize(1);
  display.setCursor(70, 22);
  display.print("AQI");

  display.setTextSize(1);
  display.setCursor(0, 48);
  display.print(g_aqiCategory);

  display.setCursor(80, 48);
  display.print("RAW:");
  display.print(raw);

  if (g_aqiValue >= 110) {
    display.setCursor(0, 56);
    display.print("! ALERT !");
  }

  display.display();
}

// ── Single buzzer beep ─────────────────────────────────────
void singleBeep() {
  digitalWrite(BUZZER_PIN, HIGH);
  delay(300);
  digitalWrite(BUZZER_PIN, LOW);
}

// ── HTTP: GET /data ────────────────────────────────────────
void handleData() {
  StaticJsonDocument<200> doc;
  doc["aqi"]      = (int)g_aqiValue;
  doc["category"] = g_aqiCategory;
  doc["alert"]    = (g_aqiValue >= 110);
  doc["ip"]       = WiFi.localIP().toString();

  String json;
  serializeJson(doc, json);

  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", json);
}

// ── HTTP: GET / ────────────────────────────────────────────
void handleRoot() {
  server.send(200, "text/plain",
    "Air Quality ESP8266 running!\n"
    "GET /data for JSON.\n"
    "IP: " + WiFi.localIP().toString());
}

// ── Setup ──────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  // OLED
  Wire.begin(4, 5);  // SDA=D2(GPIO4), SCL=D1(GPIO5)
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("SSD1306 init failed!");
    while (true);
  }
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("Connecting WiFi...");
  display.display();

  // WiFi
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("Connected! IP: ");
  Serial.println(WiFi.localIP());

  display.clearDisplay();
  display.setCursor(0, 0);
  display.print("WiFi Connected!");
  display.setCursor(0, 12);
  display.print(WiFi.localIP().toString());
  display.display();
  delay(2000);

  // HTTP routes
  server.on("/",     handleRoot);
  server.on("/data", handleData);
  server.begin();
  Serial.println("HTTP server started");
}

// ── Loop ───────────────────────────────────────────────────
void loop() {
  server.handleClient();

  unsigned long now = millis();
  if (now - g_lastRead >= READ_INTERVAL) {
    g_lastRead = now;

    int raw       = analogRead(MQ135_PIN);
    g_aqiValue    = rawToAQI(raw);
    g_aqiCategory = aqiCategory(g_aqiValue);

    Serial.printf("RAW: %d | AQI: %.1f | %s\n",
                  raw, g_aqiValue, g_aqiCategory.c_str());

    // Buzzer: beep once when AQI first hits 110
    if (g_aqiValue >= 110 && !g_buzzerFired) {
      singleBeep();
      g_buzzerFired = true;
    } else if (g_aqiValue < 110) {
      g_buzzerFired = false;
    }

    updateDisplay(raw);
  }
}
