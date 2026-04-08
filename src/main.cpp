/*
 * ESP32 Air Quality Monitor
 *
 * This program reads temperature, humidity, and particulate matter (PM)
 * data from two sensors, stores a rolling history in memory, and serves
 * an interactive dashboard with charts that you can view from any device
 * on your WiFi network.
 *
 * Hardware:
 *   - ESP32 dev board
 *   - DHT22 sensor (temperature + humidity)
 *   - PMS5003 sensor (particulate matter: PM1.0, PM2.5, PM10)
 *
 * How it works:
 *   1. On boot, the ESP32 connects to your WiFi network and starts
 *      a tiny web server on port 80 (normal HTTP).
 *   2. The main loop checks sensors and handles web requests.
 *      - The DHT22 is read every 2 seconds (its internal refresh rate).
 *      - The PMS5003 runs a duty cycle: it wakes up, runs for 30 seconds
 *        to get stable readings, then sleeps for a configurable period
 *        to save power (~100 mA). By default it sleeps for 90 seconds,
 *        giving a 2-minute measurement cycle.
 *   3. Each time a new PMS reading arrives, a data point is stored in a
 *      circular buffer in RAM. The buffer holds up to 7 days of history
 *      at a 2-minute interval (5,040 points, ~121 KB).
 *   4. When you visit the ESP32's IP address in a browser, it streams
 *      an HTML page with live readings, interactive Chart.js charts
 *      showing the full history, and a button to download all data as
 *      a CSV file.
 *   5. The page auto-refreshes every 30 seconds so you always see
 *      reasonably current data without excessive network traffic.
 *
 * Memory budget (ESP32 has ~200-250 KB free after WiFi):
 *   - History buffer: 5,040 points x 24 bytes = ~121 KB (heap-allocated)
 *   - Everything else (WiFi, stack, buffers):   ~80-130 KB
 *   This leaves comfortable headroom. If allocation fails at boot,
 *   the buffer automatically shrinks to fit available memory.
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

// --- PMS5003 Duty Cycle Configuration ---
// The PMS5003's fan and laser draw ~100 mA. Running them continuously
// wastes power and shortens the fan's lifespan. Instead, we wake the
// sensor periodically, let it stabilize, take a reading, then put it
// back to sleep. The datasheet recommends at least 30 seconds of
// run time before trusting the readings.
//
// With the defaults below the sensor is awake for 30 s out of every
// 120 s (25% duty cycle), cutting its average draw from ~100 mA to ~25 mA.
#define PMS_WAKE_MS   30000   // Run the fan/laser for 30 seconds
#define PMS_SLEEP_MS  90000   // Then sleep for 90 seconds

// --- DHT22 Read Interval ---
// The DHT22's internal sampling rate is once every 2 seconds.
// Reading it faster just returns the same value and wastes CPU time.
#define DHT_READ_MS 2000

// ============================================================
// History Buffer (Circular / Ring Buffer)
// ============================================================
//
// We store past sensor readings in a fixed-size array that wraps
// around when full. This is called a "circular buffer" or "ring
// buffer". It works like this:
//
//   - New data is written at position `historyHead`.
//   - After writing, historyHead advances by 1 (wrapping to 0
//     at the end of the array).
//   - `historyCount` tracks how many slots are filled (up to
//     HISTORY_MAX). Once full, new data overwrites the oldest.
//
// This gives us O(1) insertion with zero memory allocation --
// critical on a microcontroller with no garbage collector.
//
// Memory math:
//   Each DataPoint is 24 bytes (5 floats + 1 unsigned long).
//   With a 2-minute cycle: 720 points/day x 7 days = 5,040 points.
//   5,040 x 24 bytes = 120,960 bytes (~121 KB).
//   The ESP32 has ~200-250 KB free after WiFi, so this fits on the
//   heap. The buffer is allocated in setup() with a fallback to
//   smaller sizes if memory is tight.

#define HISTORY_MAX 5040  // Target: 7 days at 1 sample per 2-minute cycle

// Each data point stores all five sensor values plus a timestamp.
// The timestamp is "millis() / 1000" (seconds since boot) rather
// than wall-clock time because the ESP32 doesn't have a real-time
// clock. The dashboard converts this to relative time labels like
// "2h ago" for the chart axis.
struct DataPoint {
  float temp;       // Temperature in Celsius
  float humidity;   // Relative humidity in %
  float pm1_0;      // PM1.0 in ug/m3
  float pm2_5;      // PM2.5 in ug/m3
  float pm10;       // PM10 in ug/m3
  unsigned long ts;  // Seconds since boot (millis()/1000)
};

// The history array is allocated on the HEAP at startup (in setup())
// rather than as a global array. Global arrays go into the BSS segment
// of DRAM, which is limited to ~160 KB and shared with WiFi and the
// framework. The heap can use more of the ESP32's total 520 KB SRAM,
// so a ~121 KB buffer fits comfortably there but overflows BSS.
//
// We use a pointer here and call malloc() in setup(). If allocation
// fails (not enough free heap), we fall back to a smaller buffer.
DataPoint* history = nullptr;
int historyMax = HISTORY_MAX;   // Actual capacity (may shrink on alloc failure)
int historyHead = 0;            // Next position to write
int historyCount = 0;           // How many valid entries (0..historyMax)

// ============================================================
// Sensor Reading (cached values)
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
 * recordDataPoint() - Save the current sensor readings to the history buffer.
 *
 * Called once per PMS5003 measurement cycle (~every 2 minutes).
 * Uses the latest cached DHT22 values and the just-received PMS data.
 *
 * If the buffer is full (historyCount == HISTORY_MAX), the oldest
 * data point is silently overwritten -- no memory allocation needed.
 */
void recordDataPoint() {
  if (!history) return;  // Allocation failed in setup(); no history available

  DataPoint dp;
  dp.temp     = lastTemp;       // May be NAN if DHT22 hasn't read yet
  dp.humidity = lastHumidity;   // May be NAN if DHT22 hasn't read yet
  dp.pm1_0    = lastPms.PM_AE_UG_1_0;
  dp.pm2_5    = lastPms.PM_AE_UG_2_5;
  dp.pm10     = lastPms.PM_AE_UG_10_0;
  dp.ts       = millis() / 1000;

  // Write to the current head position and advance.
  history[historyHead] = dp;
  historyHead = (historyHead + 1) % historyMax;

  // Track how many slots are occupied (caps at historyMax).
  if (historyCount < historyMax) {
    historyCount++;
  }
}

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

  // --- Allocate the history buffer on the heap ---
  // We do this early, before WiFi grabs a large chunk of heap for its
  // internal buffers. If the full 5,040-point buffer doesn't fit, we
  // try progressively smaller sizes so the monitor still works (just
  // with fewer days of history).
  history = (DataPoint*)malloc(historyMax * sizeof(DataPoint));
  if (!history) {
    // Full buffer didn't fit -- try half (3.5 days)
    historyMax = HISTORY_MAX / 2;
    history = (DataPoint*)malloc(historyMax * sizeof(DataPoint));
  }
  if (!history) {
    // Still didn't fit -- try 1 day (720 points = ~17 KB)
    historyMax = 720;
    history = (DataPoint*)malloc(historyMax * sizeof(DataPoint));
  }
  if (history) {
    memset(history, 0, historyMax * sizeof(DataPoint));
    Serial.printf("History buffer: %d points (%d KB)\n",
      historyMax, (int)(historyMax * sizeof(DataPoint) / 1024));
  } else {
    historyMax = 0;
    Serial.println("WARNING: History buffer allocation failed, no charting");
  }

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

// --- PMS5003 duty-cycle state machine ---
// The sensor alternates between two states:
//   AWAKE  -- fan spinning, laser on, actively sending data frames
//   ASLEEP -- fan and laser off, drawing almost no current
// We track the state and when we entered it so we know when to switch.
bool pmsAwake = true;                // Starts awake after power-on
unsigned long pmsStateStart = 0;     // millis() when we entered the current state

// --- DHT22 read throttle ---
// Tracks the last time we polled the DHT22 so we only read it every
// DHT_READ_MS milliseconds instead of every loop iteration.
unsigned long lastDhtRead = 0;

/*
 * readSensors() - Poll both sensors and update the cache.
 *
 * DHT22: Only polled every DHT_READ_MS (2 seconds) to match the sensor's
 *        internal refresh rate. Returns NAN on failure (wiring issue, etc.).
 *        We only update the cache when BOTH temp and humidity are valid.
 *
 * PMS5003: Runs a duty cycle to save power:
 *   1. AWAKE  -- fan and laser are on. We call pms.read() to grab data.
 *               After PMS_WAKE_MS (30 s) we put the sensor to sleep.
 *   2. ASLEEP -- fan and laser are off (~0 mA). After PMS_SLEEP_MS (90 s)
 *               we wake it up and go back to step 1.
 *
 *   The first cycle after boot skips the initial sleep so you get a
 *   reading within 30 seconds of power-on.
 *
 *   When a new PMS reading arrives, we also record a data point to
 *   the history buffer for charting.
 */
void readSensors() {
  // ---- DHT22 (throttled) ----
  // Only read once every DHT_READ_MS. millis() wraps after ~50 days,
  // but unsigned subtraction handles that correctly.
  if (millis() - lastDhtRead >= DHT_READ_MS) {
    lastDhtRead = millis();

    float h = dht.readHumidity();     // Relative humidity in %
    float t = dht.readTemperature();  // Temperature in Celsius

    // isnan() checks if a float is "Not A Number" (i.e., the read failed).
    // We only update the cache when both values are valid so we never
    // display a mix of one good reading and one "nan".
    if (!isnan(t) && !isnan(h)) {
      lastTemp = t;
      lastHumidity = h;
    }
  }

  // ---- PMS5003 (duty-cycled) ----
  unsigned long elapsed = millis() - pmsStateStart;

  if (pmsAwake) {
    // The sensor is running -- try to read a data frame.
    // pms.read() is non-blocking: it returns true only when a complete
    // 32-byte frame has arrived over serial (~once per second).
    if (pms.read(data)) {
      lastPms = data;
      pmsReady = true;
    }

    // After PMS_WAKE_MS of run time, put the sensor to sleep and
    // record the latest readings to the history buffer.
    if (elapsed >= PMS_WAKE_MS) {
      // Save a data point right before sleeping -- this captures the
      // most recent (and most stable) reading from this wake cycle.
      if (pmsReady) {
        recordDataPoint();
      }

      pms.sleep();   // Sends a command over serial to turn off the fan/laser
      pmsAwake = false;
      pmsStateStart = millis();
      Serial.println("PMS5003 sleeping");
    }
  } else {
    // The sensor is asleep -- check if it's time to wake up.
    if (elapsed >= PMS_SLEEP_MS) {
      pms.wakeUp();  // Sends a command over serial to turn on the fan/laser
      pmsAwake = true;
      pmsStateStart = millis();
      Serial.println("PMS5003 waking up");
    }
  }
}

// ============================================================
// Web Server - Streaming HTML Response
// ============================================================
//
// The dashboard page is too large to fit in a single buffer because
// it includes all the history data for the charts. Instead of
// building the entire page in memory, we "stream" it: send it to
// the browser in small pieces, one after another.
//
// Think of it like reading a book aloud -- you don't memorize the
// whole book first, you read one page at a time. The browser
// assembles the pieces into the complete page.
//
// We use a small 256-byte buffer for formatting individual lines
// (like one row of chart data), send that chunk, then reuse the
// same buffer for the next line.

// Small reusable buffer for formatting individual lines of output.
// This is much more memory-efficient than building the whole page
// in one giant buffer.
//
// IMPORTANT: Every sendChunk() call must produce output shorter than
// this buffer. If a formatted string exceeds this size, vsnprintf
// silently truncates it and the HTML will be broken. When adding
// new sendChunk() calls, keep each one well under this limit --
// split large chunks into multiple calls if needed.
static char lineBuf[256];

/*
 * sendChunk() - Format a string and send it to the client immediately.
 *
 * Works like printf() but sends directly to the WiFi client instead
 * of printing to the serial monitor. Uses our small lineBuf to
 * format the string, then calls client.print() to transmit it.
 *
 * The "..." means this function accepts a variable number of arguments,
 * just like printf/snprintf. For example:
 *   sendChunk(client, "<p>Temperature: %.1f</p>", 23.4);
 */
void sendChunk(WiFiClient& client, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vsnprintf(lineBuf, sizeof(lineBuf), fmt, args);
  va_end(args);
  client.print(lineBuf);
}

/*
 * sendDashboardPage() - Stream the full HTML dashboard to the client.
 *
 * This function sends the page in sections:
 *   1. HTTP headers
 *   2. HTML head (CSS, Chart.js library link, meta-refresh)
 *   3. Current sensor readings
 *   4. Chart canvases (empty <canvas> elements the JS will draw into)
 *   5. History data as JavaScript arrays (one line per data point)
 *   6. Chart.js configuration and initialization
 *   7. CSV download button and its JavaScript
 *   8. HTML closing tags
 *
 * The history data is the bulk of the page. For 7 days of data
 * (~5,040 points), this section alone is ~100-150 KB of text.
 * Streaming lets us send it without ever holding it all in RAM.
 */
void sendDashboardPage(WiFiClient& client) {
  // ---- HTTP headers ----
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html");
  client.println("Connection: close");
  client.println();

  // ---- HTML head ----
  // We load Chart.js from a CDN (content delivery network). This is a
  // popular open-source charting library that runs in the browser. The
  // ESP32 doesn't do any chart rendering -- it just sends the data and
  // the browser's JavaScript engine does all the drawing work.
  //
  // The chartjs-adapter-date-fns import lets Chart.js understand time-
  // based X axes so it can show labels like "12:00" or "Mon".
  client.print(
    "<!DOCTYPE html><html><head>"
    "<meta charset='UTF-8'>"
    "<meta http-equiv='refresh' content='30'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Air Quality Monitor</title>"
    "<script src='https://cdn.jsdelivr.net/npm/chart.js@4'></script>"
    "<script src='https://cdn.jsdelivr.net/npm/chartjs-adapter-date-fns@3'></script>"
    "<style>"
      "body{font-family:Arial,sans-serif;margin:20px;max-width:900px;margin:0 auto;padding:20px;}"
      "h1{color:#333;} h2{color:#555;margin-top:30px;}"
      ".readings{display:flex;flex-wrap:wrap;gap:15px;margin:15px 0;}"
      ".card{background:#f8f9fa;border-radius:8px;padding:15px 20px;"
        "border-left:4px solid #4CAF50;min-width:140px;}"
      ".card.warn{border-left-color:#FF9800;}"
      ".card.error{border-left-color:#f44336;}"
      ".card .label{font-size:0.85em;color:#666;}"
      ".card .value{font-size:1.4em;font-weight:bold;color:#333;}"
      ".chart-container{position:relative;height:250px;margin:15px 0;}"
      "canvas{width:100%!important;}"
      ".btn{display:inline-block;padding:10px 20px;background:#4CAF50;"
        "color:#fff;border:none;border-radius:4px;cursor:pointer;"
        "font-size:1em;margin-top:10px;text-decoration:none;}"
      ".btn:hover{background:#388E3C;}"
      ".info{color:#888;font-size:0.85em;margin-top:5px;}"
    "</style>"
    "</head><body>"
    "<h1>ESP32 Air Quality Monitor</h1>"
  );

  // ---- Current readings cards ----
  // Show the latest values in colored cards at the top of the page.
  if (isnan(lastTemp) || isnan(lastHumidity)) {
    client.print("<div class='readings'><div class='card error'>"
      "<div class='label'>Sensor</div>"
      "<div class='value'>DHT22 error</div></div></div>");
  } else {
    sendChunk(client,
      "<div class='readings'>"
      "<div class='card'><div class='label'>Temperature</div>"
      "<div class='value'>%.1f &deg;C</div></div>"
      "<div class='card'><div class='label'>Humidity</div>"
      "<div class='value'>%.1f %%</div></div>",
      lastTemp, lastHumidity);

    if (!pmsReady) {
      client.print(
        "<div class='card warn'><div class='label'>PM Status</div>"
        "<div class='value'>Warming up...</div></div></div>");
    } else {
      // Color the PM2.5 card based on air quality level:
      //   Green  (good):     0 - 12 ug/m3
      //   Orange (moderate): 12 - 35 ug/m3
      //   Red    (unhealthy): > 35 ug/m3
      const char* pmClass = lastPms.PM_AE_UG_2_5 <= 12 ? "card" :
                            lastPms.PM_AE_UG_2_5 <= 35 ? "card warn" : "card error";
      sendChunk(client,
        "<div class='card'><div class='label'>PM1.0</div>"
        "<div class='value'>%u &micro;g/m&sup3;</div></div>",
        lastPms.PM_AE_UG_1_0);
      sendChunk(client,
        "<div class='%s'><div class='label'>PM2.5</div>"
        "<div class='value'>%u &micro;g/m&sup3;</div></div>",
        pmClass, lastPms.PM_AE_UG_2_5);
      sendChunk(client,
        "<div class='card'><div class='label'>PM10</div>"
        "<div class='value'>%u &micro;g/m&sup3;</div></div>"
        "</div>",
        lastPms.PM_AE_UG_10_0);
    }
  }

  // ---- Chart canvases ----
  // These are empty <canvas> elements. Chart.js will draw into them
  // using the data arrays we define in the <script> section below.
  // Each chart gets its own container div with a fixed height so
  // the page layout doesn't jump around as charts load.
  client.print(
    "<h2>Temperature &amp; Humidity</h2>"
    "<div class='chart-container'><canvas id='thChart'></canvas></div>"
    "<h2>Particulate Matter</h2>"
    "<div class='chart-container'><canvas id='pmChart'></canvas></div>"
  );

  // ---- History data as JavaScript arrays ----
  // We write the history data directly into a <script> block as
  // JavaScript arrays. The browser parses these and Chart.js uses
  // them to draw the line charts.
  //
  // Each data point becomes one element in each array:
  //   ts[i]   = seconds since boot (converted to a Date by JS)
  //   tp[i]   = temperature
  //   hu[i]   = humidity
  //   p1[i]   = PM1.0
  //   p25[i]  = PM2.5
  //   p10[i]  = PM10
  //
  // We iterate through the circular buffer in chronological order
  // (oldest to newest) by starting at the right offset.
  client.print("<script>\n");
  client.print("var ts=[],tp=[],hu=[],p1=[],p25=[],p10=[];\n");

  // Walk the circular buffer from oldest to newest.
  // If the buffer isn't full yet, start from index 0.
  // If it IS full, the oldest entry is at historyHead (because that's
  // the next position to be overwritten).
  unsigned long nowSec = millis() / 1000;
  int start = (historyCount < historyMax) ? 0 : historyHead;

  for (int i = 0; i < historyCount; i++) {
    int idx = (start + i) % historyMax;
    DataPoint& dp = history[idx];

    // Convert the timestamp from "seconds since boot" to
    // "seconds ago" (relative to now). The JavaScript on the
    // browser side will turn this into an absolute Date object
    // by subtracting from the current time.
    long ago = (long)(nowSec - dp.ts);

    // Send one line per data point. Each line pushes values into
    // the six arrays. We use isnan() to skip bad DHT readings --
    // Chart.js handles null gracefully by leaving gaps in the line.
    if (isnan(dp.temp)) {
      sendChunk(client,
        "ts.push(%ld);tp.push(null);hu.push(null);"
        "p1.push(%.0f);p25.push(%.0f);p10.push(%.0f);\n",
        ago, dp.pm1_0, dp.pm2_5, dp.pm10);
    } else {
      sendChunk(client,
        "ts.push(%ld);tp.push(%.1f);hu.push(%.1f);"
        "p1.push(%.0f);p25.push(%.0f);p10.push(%.0f);\n",
        ago, dp.temp, dp.humidity, dp.pm1_0, dp.pm2_5, dp.pm10);
    }
  }

  // ---- Chart.js initialization ----
  // Convert "seconds ago" timestamps into JavaScript Date objects.
  // Then create two charts: one for temp+humidity, one for PM values.
  //
  // Chart.js is configured with:
  //   - Time-based X axis (shows hours/days depending on zoom)
  //   - Responsive sizing (fills the container)
  //   - Tooltips showing exact values on hover
  //   - Smooth lines with no fill (clean look)
  //   - spanGaps:true so missing DHT readings don't break the line
  client.print(
    "var now=new Date();\n"
    "var labels=ts.map(function(s){return new Date(now.getTime()-s*1000);});\n"

    // Temperature & Humidity chart
    "new Chart(document.getElementById('thChart'),{type:'line',data:{"
      "labels:labels,datasets:["
        "{label:'Temp (\\u00B0C)',data:tp,borderColor:'#e74c3c',backgroundColor:'rgba(231,76,60,0.1)',"
          "borderWidth:1.5,pointRadius:0,tension:0.3,spanGaps:true},"
        "{label:'Humidity (%)',data:hu,borderColor:'#3498db',backgroundColor:'rgba(52,152,219,0.1)',"
          "borderWidth:1.5,pointRadius:0,tension:0.3,yAxisID:'y1',spanGaps:true}"
      "]},options:{responsive:true,maintainAspectRatio:false,interaction:{mode:'index',intersect:false},"
        "scales:{"
          "x:{type:'time',time:{tooltipFormat:'MMM d, HH:mm',displayFormats:{hour:'HH:mm',day:'MMM d'}},"
            "ticks:{maxTicksLimit:8}},"
          "y:{position:'left',title:{display:true,text:'\\u00B0C'}},"
          "y1:{position:'right',title:{display:true,text:'%'},grid:{drawOnChartArea:false}}"
        "}"
      "}});\n"

    // Particulate Matter chart
    "new Chart(document.getElementById('pmChart'),{type:'line',data:{"
      "labels:labels,datasets:["
        "{label:'PM1.0',data:p1,borderColor:'#2ecc71',borderWidth:1.5,pointRadius:0,tension:0.3},"
        "{label:'PM2.5',data:p25,borderColor:'#e67e22',borderWidth:1.5,pointRadius:0,tension:0.3},"
        "{label:'PM10',data:p10,borderColor:'#9b59b6',borderWidth:1.5,pointRadius:0,tension:0.3}"
      "]},options:{responsive:true,maintainAspectRatio:false,interaction:{mode:'index',intersect:false},"
        "scales:{"
          "x:{type:'time',time:{tooltipFormat:'MMM d, HH:mm',displayFormats:{hour:'HH:mm',day:'MMM d'}},"
            "ticks:{maxTicksLimit:8}},"
          "y:{position:'left',title:{display:true,text:'\\u03BCg/m\\u00B3'},beginAtZero:true}"
        "}"
      "}});\n"
  );

  // ---- CSV download button ----
  // When the user clicks "Download CSV", JavaScript builds a CSV
  // string from the same data arrays, creates a temporary download
  // link, and clicks it. This all happens in the browser -- the
  // ESP32 doesn't need to do any extra work.
  //
  // The CSV format is simple and opens in any spreadsheet app:
  //   seconds_ago,temperature,humidity,pm1_0,pm2_5,pm10
  client.print(
    "function downloadCSV(){"
      "var lines=['seconds_ago,temperature,humidity,pm1_0,pm2_5,pm10'];"
      "for(var i=0;i<ts.length;i++){"
        "lines.push(ts[i]+','+(tp[i]===null?'':tp[i])+','+"
          "(hu[i]===null?'':hu[i])+','+p1[i]+','+p25[i]+','+p10[i]);"
      "}"
      "var blob=new Blob([lines.join('\\n')],{type:'text/csv'});"
      "var a=document.createElement('a');"
      "a.href=URL.createObjectURL(blob);"
      "a.download='airquality.csv';"
      "a.click();"
    "}\n"
    "</script>\n"
  );

  // ---- Download button and footer ----
  sendChunk(client,
    "<button class='btn' onclick='downloadCSV()'>Download CSV</button>"
    "<p class='info'>%d data points stored (up to %d max ~ %d days). "
    "Page refreshes every 30 seconds.</p>"
    "</body></html>",
    historyCount, historyMax, historyMax / 720);
}

/*
 * sendCSVData() - Stream raw CSV data for programmatic download.
 *
 * This endpoint (GET /data.csv) returns the history buffer as a
 * plain CSV file. Useful for importing into Excel, Google Sheets,
 * Python scripts, etc. without going through the browser's
 * JavaScript-based download.
 *
 * The response includes a Content-Disposition header that tells
 * the browser to save the file as "airquality.csv" rather than
 * displaying it.
 */
void sendCSVData(WiFiClient& client) {
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/csv");
  client.println("Content-Disposition: attachment; filename=\"airquality.csv\"");
  client.println("Connection: close");
  client.println();

  // CSV header row
  client.println("seconds_ago,temperature,humidity,pm1_0,pm2_5,pm10");

  unsigned long nowSec = millis() / 1000;
  int start = (historyCount < historyMax) ? 0 : historyHead;

  for (int i = 0; i < historyCount; i++) {
    int idx = (start + i) % historyMax;
    DataPoint& dp = history[idx];
    long ago = (long)(nowSec - dp.ts);

    if (isnan(dp.temp)) {
      sendChunk(client, "%ld,,,%.0f,%.0f,%.0f\n",
        ago, dp.pm1_0, dp.pm2_5, dp.pm10);
    } else {
      sendChunk(client, "%ld,%.1f,%.1f,%.0f,%.0f,%.0f\n",
        ago, dp.temp, dp.humidity, dp.pm1_0, dp.pm2_5, dp.pm10);
    }
  }
}

/*
 * handleClient() - Check if a browser is requesting our page, and respond.
 *
 * The ESP32 runs a very simple HTTP server. When someone types the ESP32's
 * IP address into their browser, the browser opens a TCP connection and
 * sends an HTTP request (like "GET / HTTP/1.1"). We read that request
 * to determine which resource was requested:
 *
 *   GET /           -> Send the dashboard page with charts
 *   GET /data.csv   -> Send raw CSV data for download
 *   Anything else   -> Send the dashboard page (fallback)
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

  // Read the first line of the HTTP request (e.g., "GET /data.csv HTTP/1.1").
  // We check if it contains "/data.csv" to decide which response to send.
  String req = client.readStringUntil('\r');
  client.flush();  // Discard the rest of the request headers

  // Route the request to the appropriate handler.
  if (req.indexOf("/data.csv") >= 0) {
    sendCSVData(client);
  } else {
    sendDashboardPage(client);
  }
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
