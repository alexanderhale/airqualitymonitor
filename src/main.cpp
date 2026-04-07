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

void setup() {
  Serial.begin(115200);

  // PMS5003 UART
  Serial2.begin(9600, SERIAL_8N1, 16, 17);

  dht.begin();
  delay(2000);

  // WiFi
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }

  server.begin();
}

// Cached sensor readings (last-known-good values)
float lastTemp = NAN;
float lastHumidity = NAN;
bool pmsReady = false;
PMS::DATA lastPms;

String htmlPage() {
  String page = "<!DOCTYPE html><html><head>";
  page += "<meta http-equiv='refresh' content='5'>";
  page += "<style>body{font-family:Arial;margin:40px;}h1{color:#333;}.error{color:#c00;}</style>";
  page += "</head><body>";
  page += "<h1>ESP32 Air Quality Station</h1>";

  if (isnan(lastTemp) || isnan(lastHumidity)) {
    page += "<p class='error'>Temperature/Humidity sensor error</p>";
  } else {
    page += "<p><b>Temperature:</b> " + String(lastTemp) + " &deg;C</p>";
    page += "<p><b>Humidity:</b> " + String(lastHumidity) + " %</p>";
  }

  if (!pmsReady) {
    page += "<p class='error'>Particulate sensor warming up...</p>";
  } else {
    page += "<p><b>PM1.0:</b> " + String(lastPms.PM_AE_UG_1_0) + " &micro;g/m&sup3;</p>";
    page += "<p><b>PM2.5:</b> " + String(lastPms.PM_AE_UG_2_5) + " &micro;g/m&sup3;</p>";
    page += "<p><b>PM10:</b> " + String(lastPms.PM_AE_UG_10_0) + " &micro;g/m&sup3;</p>";
  }

  page += "</body></html>";
  return page;
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

  String page = htmlPage();

  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html");
  client.println("Connection: close");
  client.println();
  client.println(page);
}

void loop() {
  readSensors();
  handleClient();
  delay(100);
}