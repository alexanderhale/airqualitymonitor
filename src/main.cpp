#include <WiFi.h>
#include <Adafruit_Sensor.h>
#include <DHT.h>
#include <DHT_U.h>
#include <PMS.h>
#include "secrets.h"

#define DHTPIN 4
#define DHTTYPE DHT22

DHT dht(DHTPIN, DHTTYPE);

PMS pms(Serial2);
PMS::DATA data;

// ---- WiFi ----
const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;

WiFiServer server(80);

#define WIFI_TIMEOUT_MS 30000
#define WIFI_RECONNECT_MS 10000

void connectWiFi() {
  WiFi.begin(ssid, password);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) {
    delay(500);
  }
}

void setup() {
  Serial.begin(115200);

  // PMS5003 UART
  Serial2.begin(9600, SERIAL_8N1, 16, 17);

  dht.begin();
  delay(2000);

  // WiFi
  WiFi.setAutoReconnect(true);
  connectWiFi();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi connect failed, rebooting...");
    ESP.restart();
  }

  server.begin();
}

// Cached sensor readings (last-known-good values)
float lastTemp = NAN;
float lastHumidity = NAN;
bool pmsReady = false;
PMS::DATA lastPms;

static char pageBuf[1024];

const char* htmlPage() {
  char tempStr[64];
  char humStr[64];
  char pmsStr[256];

  if (isnan(lastTemp) || isnan(lastHumidity)) {
    snprintf(tempStr, sizeof(tempStr), "<p class='error'>Temperature/Humidity sensor error</p>");
    humStr[0] = '\0';
  } else {
    snprintf(tempStr, sizeof(tempStr), "<p><b>Temperature:</b> %.1f &deg;C</p>", lastTemp);
    snprintf(humStr, sizeof(humStr), "<p><b>Humidity:</b> %.1f %%</p>", lastHumidity);
  }

  if (!pmsReady) {
    snprintf(pmsStr, sizeof(pmsStr), "<p class='error'>Particulate sensor warming up...</p>");
  } else {
    snprintf(pmsStr, sizeof(pmsStr),
      "<p><b>PM1.0:</b> %u &micro;g/m&sup3;</p>"
      "<p><b>PM2.5:</b> %u &micro;g/m&sup3;</p>"
      "<p><b>PM10:</b> %u &micro;g/m&sup3;</p>",
      lastPms.PM_AE_UG_1_0, lastPms.PM_AE_UG_2_5, lastPms.PM_AE_UG_10_0);
  }

  snprintf(pageBuf, sizeof(pageBuf),
    "<!DOCTYPE html><html><head>"
    "<meta http-equiv='refresh' content='5'>"
    "<style>body{font-family:Arial;margin:40px;}h1{color:#333;}.error{color:#c00;}</style>"
    "</head><body>"
    "<h1>ESP32 Air Quality Station</h1>"
    "%s%s%s"
    "</body></html>",
    tempStr, humStr, pmsStr);

  return pageBuf;
}

void readSensors() {
  float h = dht.readHumidity();
  float t = dht.readTemperature();

  // Only update cache if readings are valid
  if (!isnan(t) && !isnan(h)) {
    lastTemp = t;
    lastHumidity = h;
  }

  if (pms.read(data)) {
    lastPms = data;
    pmsReady = true;
  }
}

void handleClient() {
  WiFiClient client = server.available();
  if (!client) return;

  // Wait for client data with timeout (2 seconds)
  unsigned long timeout = millis() + 2000;
  while (!client.available() && millis() < timeout) {
    delay(1);
  }
  if (!client.available()) {
    client.stop();
    return;
  }

  String req = client.readStringUntil('\r');
  client.flush();

  const char* page = htmlPage();

  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html");
  client.println("Connection: close");
  client.println();
  client.println(page);
}

unsigned long lastWiFiCheck = 0;

void loop() {
  readSensors();
  handleClient();

  // Reconnect WiFi if dropped
  if (millis() - lastWiFiCheck > WIFI_RECONNECT_MS) {
    lastWiFiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi lost, reconnecting...");
      connectWiFi();
    }
  }

  delay(100);
}