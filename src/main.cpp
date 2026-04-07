/*
 * ESP32 Air Quality Monitor
 *
 * This program reads temperature, humidity, and particulate matter (PM)
 * data from two sensors and serves the readings as a web page you can
 * view from any device on your WiFi network.
 *
 * Hardware:
 *   - ESP32 dev board
 *   - DHT22 sensor (temperature + humidity)
 *   - PMS5003 sensor (particulate matter: PM1.0, PM2.5, PM10)
 *
 * How it works:
 *   1. On boot, the ESP32 connects to your WiFi network and starts
 *      a tiny web server on port 80 (normal HTTP).
 *   2. Every 100ms, the main loop reads both sensors and checks if
 *      anyone is trying to view the web page.
 *   3. When you visit the ESP32's IP address in a browser, it builds
 *      an HTML page with the latest readings and sends it back.
 *   4. The page auto-refreshes every 5 seconds so you always see
 *      current data.
 */

// --- Libraries ---
// WiFi.h:            Built-in ESP32 WiFi support
// Adafruit_Sensor.h: Common interface used by many Adafruit sensor libraries
// DHT.h / DHT_U.h:   Driver for DHT11/DHT22 temperature & humidity sensors
// PMS.h:             Driver for Plantower PMS5003 particulate matter sensor
// secrets.h:         Your WiFi SSID and password (not committed to git)

#include <WiFi.h>
#include <Adafruit_Sensor.h>
#include <DHT.h>
#include <DHT_U.h>
#include <PMS.h>
#include "secrets.h"

// --- Pin & Sensor Configuration ---
// DHTPIN:  The GPIO pin the DHT22 data wire is connected to.
// DHTTYPE: Which DHT sensor variant we're using (DHT11, DHT21, or DHT22).
#define DHTPIN 4
#define DHTTYPE DHT22

// Create the DHT sensor object. This handles all the timing-sensitive
// communication with the DHT22 over a single data wire.
DHT dht(DHTPIN, DHTTYPE);

// The PMS5003 communicates over UART (serial). We use the ESP32's second
// hardware serial port (Serial2) so it doesn't conflict with the USB
// serial we use for debugging (Serial).
PMS pms(Serial2);
PMS::DATA data;  // Struct that holds the latest particulate reading

// --- WiFi Configuration ---
// These values come from include/secrets.h.
const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;

// Start a web server on port 80 (the default HTTP port).
// This means you can visit the ESP32's IP address in any browser
// without specifying a port number.
WiFiServer server(80);

// How long to wait for WiFi before giving up (milliseconds).
#define WIFI_TIMEOUT_MS 30000    // 30 seconds on initial connect
#define WIFI_RECONNECT_MS 10000  // Check connection every 10 seconds

/*
 * connectWiFi() - Attempt to join the WiFi network.
 *
 * Tries for up to WIFI_TIMEOUT_MS milliseconds. If it connects, great.
 * If not, the caller decides what to do (reboot, retry, etc.).
 *
 * millis() returns the number of milliseconds since the ESP32 booted.
 * We use it instead of delay() so we can check a timeout without
 * blocking forever.
 */
void connectWiFi() {
  WiFi.begin(ssid, password);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) {
    delay(500);  // Brief pause between connection attempts
  }
}

/*
 * setup() - Runs once when the ESP32 powers on or resets.
 *
 * This is where we initialize all the hardware and connect to WiFi.
 * Nothing else runs until setup() finishes.
 */
void setup() {
  // Start the USB serial connection at 115200 baud. This lets us print
  // debug messages that you can read in the PlatformIO Serial Monitor.
  Serial.begin(115200);

  // Initialize the PMS5003 serial connection.
  // The PMS5003 communicates at 9600 baud with standard settings (8 data
  // bits, no parity, 1 stop bit). GPIO 16 = RX, GPIO 17 = TX.
  Serial2.begin(9600, SERIAL_8N1, 16, 17);

  // Initialize the DHT22 sensor and give it 2 seconds to stabilize.
  // The DHT22 needs a moment after power-on before it gives valid readings.
  dht.begin();
  delay(2000);

  // Connect to WiFi. setAutoReconnect(true) tells the ESP32's WiFi stack
  // to automatically try to reconnect if the connection drops briefly.
  // We also have our own reconnect logic in loop() as a safety net.
  WiFi.setAutoReconnect(true);
  connectWiFi();

  // If WiFi didn't connect within the timeout, reboot and try again.
  // This handles situations like the router not being ready yet after
  // a power outage.
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi connect failed, rebooting...");
    ESP.restart();
  }

  // Print the IP address so you know where to point your browser.
  // Open the PlatformIO Serial Monitor to see this.
  Serial.print("Connected! Open http://");
  Serial.println(WiFi.localIP());

  // Start listening for incoming HTTP connections.
  server.begin();
}

// ============================================================
// Sensor Reading
// ============================================================

// We cache the last successful sensor readings in global variables.
// This way the web page always has *something* to display, even if
// a sensor temporarily fails to read. NAN (Not A Number) is used
// as the initial "no data yet" value for the DHT readings.
float lastTemp = NAN;
float lastHumidity = NAN;
bool pmsReady = false;    // Has the PMS5003 delivered at least one reading?
PMS::DATA lastPms;        // Last successful particulate reading

/*
 * readSensors() - Poll both sensors and update the cache.
 *
 * DHT22: Returns NAN on failure (wiring issue, sensor busy, etc.).
 *        We only update the cache when BOTH temp and humidity are valid.
 *
 * PMS5003: pms.read() is non-blocking -- it returns true only when a
 *          complete new data frame has arrived over serial. The sensor
 *          sends a new frame roughly every 1 second. Between frames,
 *          read() returns false and we just keep the previous values.
 */
void readSensors() {
  float h = dht.readHumidity();     // Relative humidity in %
  float t = dht.readTemperature();  // Temperature in Celsius

  // isnan() checks if a float is "Not A Number" (i.e., the read failed).
  // We only update the cache when both values are valid so we never
  // display a mix of one good reading and one "nan".
  if (!isnan(t) && !isnan(h)) {
    lastTemp = t;
    lastHumidity = h;
  }

  // Check if the PMS5003 has sent a new data frame.
  if (pms.read(data)) {
    lastPms = data;
    pmsReady = true;
  }
}

// ============================================================
// Web Page Generation
// ============================================================

// We use a fixed-size buffer on the stack for the HTML page instead of
// Arduino's String class. String concatenation (+=) causes repeated heap
// allocations that fragment memory over time on the ESP32. snprintf()
// writes directly into a pre-allocated buffer -- no fragmentation.
static char pageBuf[1024];

/*
 * htmlPage() - Build the HTML dashboard from the latest sensor cache.
 *
 * Returns a pointer to the static buffer containing the complete HTML.
 * The page includes:
 *   - Temperature and humidity (or an error message if sensor failed)
 *   - PM1.0, PM2.5, and PM10 readings (or "warming up" if no data yet)
 *   - Auto-refresh every 5 seconds via <meta http-equiv='refresh'>
 */
const char* htmlPage() {
  // Build each section into small temporary buffers first, then combine
  // them into the final page. This keeps each snprintf() call readable.
  char tempStr[64];
  char humStr[64];
  char pmsStr[256];

  // Temperature & Humidity section
  if (isnan(lastTemp) || isnan(lastHumidity)) {
    // No valid reading yet -- show an error message in red
    snprintf(tempStr, sizeof(tempStr), "<p class='error'>Temperature/Humidity sensor error</p>");
    humStr[0] = '\0';  // Empty string -- nothing to show for humidity
  } else {
    // %.1f formats the float to 1 decimal place (e.g., "23.4")
    snprintf(tempStr, sizeof(tempStr), "<p><b>Temperature:</b> %.1f &deg;C</p>", lastTemp);
    snprintf(humStr, sizeof(humStr), "<p><b>Humidity:</b> %.1f %%</p>", lastHumidity);
  }

  // Particulate Matter section
  if (!pmsReady) {
    // The PMS5003 needs ~30 seconds after power-on to stabilize its
    // internal fan and laser. Until we get the first reading, show
    // a "warming up" message.
    snprintf(pmsStr, sizeof(pmsStr), "<p class='error'>Particulate sensor warming up...</p>");
  } else {
    // PM values are unsigned integers representing micrograms per
    // cubic meter of air (ug/m3). %u formats an unsigned int.
    //   PM1.0  = particles <= 1.0 micrometers
    //   PM2.5  = particles <= 2.5 micrometers (the most health-relevant)
    //   PM10   = particles <= 10 micrometers
    snprintf(pmsStr, sizeof(pmsStr),
      "<p><b>PM1.0:</b> %u &micro;g/m&sup3;</p>"
      "<p><b>PM2.5:</b> %u &micro;g/m&sup3;</p>"
      "<p><b>PM10:</b> %u &micro;g/m&sup3;</p>",
      lastPms.PM_AE_UG_1_0, lastPms.PM_AE_UG_2_5, lastPms.PM_AE_UG_10_0);
  }

  // Assemble the full HTML page.
  // The <meta refresh> tag tells the browser to reload the page every
  // 5 seconds, so the readings update automatically without JavaScript.
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

// ============================================================
// Web Server
// ============================================================

/*
 * handleClient() - Check if a browser is requesting our page, and respond.
 *
 * The ESP32 runs a very simple HTTP server. When someone types the ESP32's
 * IP address into their browser, the browser opens a TCP connection and
 * sends an HTTP request (like "GET / HTTP/1.1"). We read that request
 * (we don't actually parse it -- we serve the same page for everything),
 * build the HTML, and send it back with the proper HTTP headers.
 *
 * This function is non-blocking: if nobody is connecting right now, it
 * returns immediately so the main loop can keep reading sensors.
 */
void handleClient() {
  // Check if a client (browser) is trying to connect. Returns immediately
  // with an empty client object if nobody is waiting.
  WiFiClient client = server.available();
  if (!client) return;

  // A client connected, but we need to wait for it to send its HTTP
  // request. We give it up to 2 seconds. This timeout protects against
  // port scanners or broken connections that connect but never send data,
  // which would otherwise freeze the ESP32.
  unsigned long timeout = millis() + 2000;
  while (!client.available() && millis() < timeout) {
    delay(1);
  }
  if (!client.available()) {
    // Timed out -- close the connection and move on.
    client.stop();
    return;
  }

  // Read the first line of the HTTP request (e.g., "GET / HTTP/1.1").
  // We don't use it, but we need to consume it so the client is ready
  // to receive our response.
  String req = client.readStringUntil('\r');
  client.flush();  // Discard the rest of the request headers

  // Build the HTML page from cached sensor data and send it.
  const char* page = htmlPage();

  // Send standard HTTP response headers. "Connection: close" tells the
  // browser we'll close the connection after sending (no keep-alive).
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html");
  client.println("Connection: close");
  client.println();       // Blank line separates headers from body
  client.println(page);   // The actual HTML content
}

// ============================================================
// Main Loop
// ============================================================

// Tracks when we last checked the WiFi connection status.
unsigned long lastWiFiCheck = 0;

/*
 * loop() - Runs repeatedly after setup() finishes.
 *
 * The Arduino framework calls this function in an infinite loop.
 * Each iteration:
 *   1. Reads sensor data (fast, non-blocking)
 *   2. Handles any waiting web client (fast if nobody is connecting)
 *   3. Periodically checks if WiFi is still connected
 *   4. Pauses 100ms to avoid busy-looping the CPU
 */
void loop() {
  readSensors();
  handleClient();

  // Every WIFI_RECONNECT_MS (10 seconds), verify we're still connected.
  // WiFi.setAutoReconnect(true) handles most drops, but this is a
  // safety net for cases where the auto-reconnect doesn't kick in
  // (e.g., router rebooted, DHCP lease expired).
  if (millis() - lastWiFiCheck > WIFI_RECONNECT_MS) {
    lastWiFiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi lost, reconnecting...");
      connectWiFi();
    }
  }

  delay(100);  // Small pause to keep loop running ~10 times per second
}