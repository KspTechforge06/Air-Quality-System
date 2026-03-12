/*
 * ============================================================
 *  AirWatch — ESP8266 + MQ135 + DHT11 + SSD1306 + Buzzer
 * ============================================================
 *  Wiring:
 *    MQ135  AOUT → A0  (via 10kΩ+10kΩ voltage divider)
 *    DHT11  DATA → D6  (GPIO12)
 *    OLED   SDA  → D2  (GPIO4)
 *    OLED   SCL  → D1  (GPIO5)
 *    Buzzer +    → D5  (GPIO14)
 *
 *  Libraries (Arduino Library Manager):
 *    - ESP8266WiFi        (built-in)
 *    - ESP8266WebServer   (built-in)
 *    - Adafruit_SSD1306
 *    - Adafruit_GFX
 *    - DHT sensor library  (by Adafruit)
 *    - ArduinoJson v6
 * ============================================================
 */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>
#include <ArduinoJson.h>

// ── Wi-Fi credentials ──────────────────────────────────────
const char* WIFI_SSID     = "";      // <-- change this
const char* WIFI_PASSWORD = "";  // <-- change this
// ── Hardware pins ──────────────────────────────────────────
#define MQ135_PIN   A0
#define BUZZER_PIN  14    // GPIO14 d5
#define DHT_PIN     12    // GPIO12 d6
#define DHT_TYPE    DHT11

// ── OLED ───────────────────────────────────────────────────
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT  64
#define OLED_RESET     -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ── Sensors & server ───────────────────────────────────────
DHT dht(DHT_PIN, DHT_TYPE);
ESP8266WebServer server(80);

// ── MQ135 calibration ─────────────────────────────────────
#define CALIBRATION_MODE  false
#define MQ135_RZERO       28.4
#define MQ135_RLOAD       10.0
#define VCC               1.0
#define ADC_MAX           1023.0

// ── Global state ───────────────────────────────────────────
float         g_aqi      = 0;
float         g_ppm      = 0;
String        g_cat      = "Good";
float         g_temp     = 0;
float         g_hum      = 0;
bool          g_buzzerFired = false;
unsigned long g_lastRead    = 0;
const unsigned long READ_INTERVAL = 2000;

// ── Helpers ────────────────────────────────────────────────
float mapf(float x, float a, float b, float c, float d) {
  return (x - a) * (d - c) / (b - a) + c;
}

float rawToPPM(int raw) {
  float vout  = (raw / ADC_MAX) * VCC;
  if (vout <= 0.001) vout = 0.001;
  float rs    = ((VCC - vout) / vout) * MQ135_RLOAD;
  float ratio = rs / MQ135_RZERO;
  return 116.6020682 * pow(ratio, -2.769034857);
}

float ppmToAQI(float ppm) {
  float aqi;
  if      (ppm < 400)  aqi = mapf(ppm,    0,  400,   0,  50);
  else if (ppm < 700)  aqi = mapf(ppm,  400,  700,  50, 100);
  else if (ppm < 1000) aqi = mapf(ppm,  700, 1000, 100, 150);
  else if (ppm < 2000) aqi = mapf(ppm, 1000, 2000, 150, 200);
  else if (ppm < 5000) aqi = mapf(ppm, 2000, 5000, 200, 300);
  else                 aqi = 301;
  return constrain(aqi, 0, 500);
}

String aqiCat(float aqi) {
  if (aqi <= 50)  return "Good";
  if (aqi <= 100) return "Moderate";
  if (aqi <= 150) return "Sensitive";
  if (aqi <= 200) return "Unhealthy";
  if (aqi <= 300) return "V.Unhealthy";
  return "Hazardous";
}

// ── OLED ───────────────────────────────────────────────────
/*
  Layout (128x64):
  ┌──────────────────────────────┐
  │ AIR QUALITY MONITOR          │  row 0  (size 1)
  │──────────────────────────────│  line y=9
  │  87          AQI             │  row 14 (size 3 + label)
  │──────────────────────────────│  line y=40
  │ Moderate                     │  row 43 (size 1)
  │ 28.5°C  65%RH                │  row 53 (size 1)
  └──────────────────────────────┘
*/
void updateDisplay(int raw) {
  display.clearDisplay();

  // Title
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print("AIR QUALITY MONITOR");
  display.drawLine(0, 9, 127, 9, SSD1306_WHITE);

  if (CALIBRATION_MODE) {
    float vout  = (raw / ADC_MAX) * VCC;
    if (vout <= 0.001) vout = 0.001;
    float rs    = ((VCC - vout) / vout) * MQ135_RLOAD;
    float rzero = rs * pow(3.6, 1.0 / 2.769);
    display.setCursor(0, 14); display.print("CALIBRATION MODE");
    display.setCursor(0, 26); display.print("RAW:  "); display.print(raw);
    display.setCursor(0, 38); display.print("Rs:   "); display.print(rs, 2);
    display.setCursor(0, 50); display.print("R0:   "); display.print(rzero, 2);
    display.display();
    return;
  }

  // Big AQI number
  display.setTextSize(3);
  display.setCursor(4, 13);
  display.print((int)g_aqi);

  // AQI label
  display.setTextSize(1);
  display.setCursor(70, 17);
  display.print("AQI");

  // PPM small
  display.setCursor(70, 29);
  display.print((int)g_ppm);
  display.print("ppm");

  // Divider
  display.drawLine(0, 40, 127, 40, SSD1306_WHITE);

  // Category (with alert marker)
  display.setTextSize(1);
  display.setCursor(0, 43);
  if (g_aqi >= 110) display.print("!");
  display.print(g_cat);

  // Temp & Humidity
  display.setCursor(0, 54);
  if (isnan(g_temp) || isnan(g_hum)) {
    display.print("DHT11: read error");
  } else {
    display.print(g_temp, 1);
    display.print((char)247);   // degree symbol
    display.print("C  ");
    display.print(g_hum, 0);
    display.print("%RH");
  }

  display.display();
}

// ── Buzzer ─────────────────────────────────────────────────
void singleBeep() {
  digitalWrite(BUZZER_PIN, HIGH);
  delay(300);
  digitalWrite(BUZZER_PIN, LOW);
}

// ── HTTP /data ─────────────────────────────────────────────
void handleData() {
  StaticJsonDocument<300> doc;
  doc["aqi"]      = (int)g_aqi;
  doc["ppm"]      = (int)g_ppm;
  doc["category"] = g_cat;
  doc["alert"]    = (g_aqi >= 110);
  doc["temp"]     = isnan(g_temp) ? -1 : round(g_temp * 10) / 10.0;
  doc["humidity"] = isnan(g_hum)  ? -1 : round(g_hum);
  doc["ip"]       = WiFi.localIP().toString();

  String json;
  serializeJson(doc, json);
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", json);
}

void handleRoot() {
  server.send(200, "text/plain",
    "AirWatch ESP8266\nGET /data for JSON\nIP: " + WiFi.localIP().toString());
}

// ── Setup ──────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(100);

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  dht.begin();

  Wire.begin(4, 5);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED init failed!");
    while (true);
  }
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  if (CALIBRATION_MODE) {
    Serial.println("=== CALIBRATION MODE — place in fresh air, wait 5 min ===");
    display.setTextSize(1);
    display.setCursor(0, 0); display.print("CALIBRATION MODE");
    display.setCursor(0, 12); display.print("Fresh air needed!");
    display.display();
    return;
  }

  display.setTextSize(1);
  display.setCursor(0, 0); display.print("Connecting WiFi...");
  display.display();

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting");
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println("\nConnected! IP: " + WiFi.localIP().toString());

  display.clearDisplay();
  display.setCursor(0, 0); display.print("WiFi Connected!");
  display.setCursor(0, 12); display.print(WiFi.localIP().toString());
  display.display();
  delay(2000);

  server.on("/",     handleRoot);
  server.on("/data", handleData);
  server.begin();
  Serial.println("HTTP server started");
}

// ── Loop ───────────────────────────────────────────────────
void loop() {
  if (!CALIBRATION_MODE) server.handleClient();

  unsigned long now = millis();
  if (now - g_lastRead >= READ_INTERVAL) {
    g_lastRead = now;

    int raw  = analogRead(MQ135_PIN);

    // DHT11 — read every cycle (it handles its own 1s minimum internally)
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t)) g_temp = t;
    if (!isnan(h)) g_hum  = h;

    if (CALIBRATION_MODE) {
      float vout  = (raw / ADC_MAX) * VCC;
      if (vout <= 0.001) vout = 0.001;
      float rs    = ((VCC - vout) / vout) * MQ135_RLOAD;
      float rzero = rs * pow(3.6, 1.0 / 2.769);
      Serial.printf("RAW: %d | Rs: %.2f kOhm | RZERO: %.2f\n", raw, rs, rzero);
      updateDisplay(raw);
      return;
    }

    g_ppm = rawToPPM(raw);
    g_aqi = ppmToAQI(g_ppm);
    g_cat = aqiCat(g_aqi);

    Serial.printf("RAW: %d | PPM: %.1f | AQI: %.1f | %s | Temp: %.1f°C | Hum: %.0f%%\n",
                  raw, g_ppm, g_aqi, g_cat.c_str(), g_temp, g_hum);

    // Buzzer
    if (g_aqi >= 110 && !g_buzzerFired) {
      singleBeep();
      g_buzzerFired = true;
    } else if (g_aqi < 110) {
      g_buzzerFired = false;
    }

    updateDisplay(raw);
  }
}
