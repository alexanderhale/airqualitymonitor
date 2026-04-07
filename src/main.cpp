#include <WiFi.h>
#include <Adafruit_Sensor.h>
#include <DHT.h>
#include <DHT_U.h>
#include <PMS.h>

#define DHTPIN 4
#define DHTTYPE DHT22

DHT dht(DHTPIN, DHTTYPE);

PMS pms(Serial2);
PMS::DATA data;

// ---- WiFi ----
const char* ssid = "YOUR_WIFI_NAME";
const char* password = "YOUR_WIFI_PASSWORD";

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

String htmlPage(float t, float h, PMS::DATA d) {
  String page = "<!DOCTYPE html><html><head>";
  page += "<meta http-equiv='refresh' content='5'>";
  page += "<style>body{font-family:Arial;margin:40px;}h1{color:#333;}</style>";
  page += "</head><body>";
  page += "<h1>ESP32 Air Quality Station</h1>";
  page += "<p><b>Temperature:</b> " + String(t) + " °C</p>";
  page += "<p><b>Humidity:</b> " + String(h) + " %</p>";
  page += "<p><b>PM1.0:</b> " + String(d.PM_AE_UG_1_0) + " µg/m³</p>";
  page += "<p><b>PM2.5:</b> " + String(d.PM_AE_UG_2_5) + " µg/m³</p>";
  page += "<p><b>PM10:</b> " + String(d.PM_AE_UG_10_0) + " µg/m³</p>";
  page += "</body></html>";
  return page;
}

void loop() {
  float h = dht.readHumidity();
  float t = dht.readTemperature();

  if (pms.read(data)) {
    WiFiClient client = server.available();
    if (client) {
      while (!client.available()) delay(1);

      String req = client.readStringUntil('\r');
      client.flush();

      String page = htmlPage(t, h, data);

      client.println("HTTP/1.1 200 OK");
      client.println("Content-Type: text/html");
      client.println("Connection: close");
      client.println();
      client.println(page);
    }
  }

  delay(2000);
}