# ESP32 Air Quality Monitor

An air quality monitoring station built with an ESP32, a DHT22 temperature/humidity sensor, and a PMS5003 particulate matter sensor. It serves a live dashboard over WiFi that you can view from any device on your network.

## What It Does

The ESP32 reads sensor data and hosts a tiny web page that auto-refreshes every 30 seconds. Open the page on your phone, tablet, or computer to see:

- **Temperature** (Celsius)
- **Humidity** (%)
- **PM1.0** -- particles <= 1.0 micrometers (micrograms per cubic meter)
- **PM2.5** -- particles <= 2.5 micrometers (the most health-relevant measurement)
- **PM10** -- particles <= 10 micrometers

PM2.5 is the number most commonly reported in air quality forecasts. Levels below 12 ug/m3 are considered good; above 35 is unhealthy for sensitive groups; above 55 is unhealthy for everyone.

## Prerequisites

### Hardware

| Component | Description | Approx. Cost (in 2026) |
|-----------|-------------|-------------|
| **ESP32 dev board** | Any ESP32-WROOM-32 based board (e.g., ESP32-DevKitC, NodeMCU-32S) | $5-10 |
| **DHT22 sensor** | Temperature and humidity sensor (also sold as AM2302) | $3-5 |
| **PMS5003 sensor** | Plantower laser particulate matter sensor | $15-25 |
| **Breadboard** | Standard half-size or full-size solderless breadboard | $3-5 |
| **Jumper wires** | Male-to-female and male-to-male dupont wires | $3-5 |
| **Micro-USB cable** | For programming and powering the ESP32 | You probably have one |

**Optional but helpful:**
- 10K-ohm resistor (pull-up for the DHT22 data line -- many DHT22 breakout boards have this built in)
- PMS5003 breakout/adapter board (the raw sensor has a 1.27mm pitch connector that's hard to breadboard directly).
   - Alternatively, you can cut the connector off one end and insert each strand of the wire individually into the breadboard.

### Software

| Software | Purpose |
|----------|---------|
| [Visual Studio Code](https://code.visualstudio.com/) | Code editor |
| [PlatformIO IDE extension](https://platformio.org/install/ide?install=vscode) | Build system, library manager, serial monitor for embedded development |
| USB-to-serial driver | Your ESP32 board may need the [CP2102](https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers) or [CH340](http://www.wch-ic.com/downloads/CH341SER_MAC_ZIP.html) driver depending on which USB chip it uses |

## Wiring

### DHT22 to ESP32

The DHT22 has 3 or 4 pins (depending on whether you have a bare sensor or a breakout board). If your board has 3 pins, the pull-up resistor is built in.

```
DHT22          ESP32
------         -----
VCC  --------> 3V3
DATA --------> GPIO 4
GND  --------> GND
```

If you have a bare 4-pin DHT22 (no breakout board), add a 10K-ohm resistor between the DATA pin and VCC (3.3V). This is a pull-up resistor that keeps the data line stable.

```
        3V3
         |
        [10K]
         |
DATA ----+--------> GPIO 4
```

### PMS5003 to ESP32

The PMS5003 uses a serial (UART) connection. It has an 8-pin connector, but you only need 4 wires:

```
PMS5003        ESP32
-------        -----
VCC  ---------> 5V  (VIN pin -- the PMS5003 needs 5V, not 3.3V!)
GND  ---------> GND
TX   ---------> GPIO 16 (ESP32 RX2)
RX   ---------> GPIO 17 (ESP32 TX2)
```

**Important notes:**
- The PMS5003 requires **5V power**. Connect it to the ESP32's VIN/5V pin, NOT the 3.3V pin. The 3.3V pin cannot supply enough current for the PMS5003's fan and laser.
- The PMS5003's TX pin connects to the ESP32's RX pin and vice versa (they cross over).
- Leave the other PMS5003 pins (SET, RESET, etc.) unconnected.

### Complete Wiring Diagram

```
                         +------------------+
                         |      ESP32       |
                         |                  |
  DHT22 VCC  ---------->| 3V3              |
  DHT22 DATA ---------->| GPIO 4           |
  DHT22 GND  ---------->| GND              |
                         |                  |
  PMS5003 VCC --------->| VIN (5V)         |
  PMS5003 GND --------->| GND              |
  PMS5003 TX  --------->| GPIO 16 (RX2)    |
  PMS5003 RX  --------->| GPIO 17 (TX2)    |
                         +------------------+
```

## Software Setup

### 1. Install VS Code and PlatformIO

1. Download and install [Visual Studio Code](https://code.visualstudio.com/).
2. Open VS Code, go to the Extensions panel.
3. Search for **"PlatformIO IDE"** and click **Install**.
4. Wait for PlatformIO to finish installing (it downloads its own Python environment and tools -- this can take a few minutes the first time).
5. You'll see a new PlatformIO icon (alien head) in the left sidebar when it's ready.

### 2. Clone or Download This Project

```bash
git clone <this-repo-url>
cd airqualitymonitor
```

Or download and extract the ZIP.

### 3. Open the Project in VS Code

```
File -> Open Folder -> select the airqualitymonitor folder
```

PlatformIO will automatically detect the `platformio.ini` file and configure the project. It will download the required libraries (DHT sensor library, PMS Library, Adafruit Unified Sensor) automatically the first time.

### 4. Set Up Your WiFi Credentials

The project keeps WiFi credentials in a separate file that is not committed to git.

1. Open `include/secrets.h.example` in VS Code.
2. Copy it to `include/secrets.h`:
   ```bash
   cp include/secrets.h.example include/secrets.h
   ```
3. Edit `include/secrets.h` and replace the placeholder values with your real WiFi name and password:
   ```cpp
   #define WIFI_SSID "YourNetworkName"
   #define WIFI_PASSWORD "YourPassword"
   ```
4. Save the file.

> **Note:** `include/secrets.h` is listed in `.gitignore` so it won't be accidentally committed. Never share this file.

### 5. Build the Project

Click the **checkmark icon** in the PlatformIO toolbar at the bottom of VS Code (or press `Cmd+Shift+B` / `Ctrl+Shift+B`).

PlatformIO will:
- Download the ESP32 toolchain (first time only)
- Compile all source files and libraries
- Report success or any errors in the terminal

### 6. Upload to the ESP32

1. Connect your ESP32 to your computer via USB.
2. Click the **right-arrow icon** in the PlatformIO toolbar at the bottom of VS Code. This builds the project and flashes it to the ESP32.
3. If PlatformIO can't find your board, you may need to:
   - Install the USB driver for your board's chip (CP2102 or CH340 -- see Prerequisites)
   - Manually select the serial port: click the **plug icon** in the PlatformIO toolbar and choose the correct port

### 7. Find the ESP32's IP Address

1. After uploading, click the **plug icon** in the PlatformIO toolbar to open the Serial Monitor.
2. Set the baud rate to **115200** (dropdown in the top-right of the monitor panel).
3. Press the **RST** (reset) button on your ESP32 board.
4. You should see output like:
   ```
   Connected! Open http://192.168.1.42
   ```
5. Note this IP address -- you'll use it to view the dashboard.

> **Tip:** If you don't see any output, make sure the baud rate is set to 115200 and try pressing the reset button again.

## WiFi Setup

### Standard Home Network

Most home WiFi networks work out of the box. Just enter your SSID and password in `secrets.h`.

### Networks with Captive Portals (hotels, offices)

The ESP32 cannot handle captive portal login pages. If your network requires a web-based login (common in hotels and some offices), the ESP32 won't be able to connect. Options:
- Use a mobile hotspot from your phone
- Use a dedicated WiFi router that you control
- Some enterprise networks allow MAC address registration -- contact your IT department

### 5 GHz vs 2.4 GHz

The ESP32 only supports **2.4 GHz WiFi**. If your router has separate 2.4 GHz and 5 GHz networks (e.g., "MyNetwork" and "MyNetwork-5G"), make sure you use the 2.4 GHz SSID in `secrets.h`. If your router uses a single combined SSID, the ESP32 will automatically connect on the 2.4 GHz band.

### Static IP (Optional)

By default, the ESP32 gets a dynamic IP address from your router via DHCP. This means the IP might change after a router reboot. If you want a fixed address, you can either:
- Assign a DHCP reservation in your router's admin panel (recommended -- no code changes needed)
- Add static IP configuration in the code (search for `WiFi.config()` in the ESP32 WiFi documentation)

## Accessing the Dashboard

1. Make sure your phone/computer is on the **same WiFi network** as the ESP32.
2. Open any web browser.
3. Type the ESP32's IP address in the address bar (e.g., `http://192.168.1.42`).
4. The dashboard loads and auto-refreshes every 30 seconds.

### What You'll See

The dashboard displays a clean, minimal page with:

**Normal operation:**
```
ESP32 Air Quality Station

Temperature: 23.4 C
Humidity: 45.2 %
PM1.0: 5 ug/m3
PM2.5: 8 ug/m3
PM10: 12 ug/m3
```

**Sensor errors:** If the DHT22 can't be read (bad wiring, sensor failure), you'll see a red "Temperature/Humidity sensor error" message instead of the values. The particulate readings will still display normally.

**PMS5003 warming up:** The PMS5003 needs about 30 seconds after power-on to stabilize its fan and laser. During this time, you'll see a "Particulate sensor warming up..." message instead of PM values.

### Understanding the Readings

| Measurement | What It Means | Good | Moderate | Unhealthy |
|-------------|---------------|------|----------|-----------|
| **Temperature** | Air temperature | -- | -- | -- |
| **Humidity** | Relative humidity | 30--50% | 50--70% | >70% or <30% |
| **PM2.5** | Fine particle pollution | 0--12 ug/m3 | 12--35 ug/m3 | >35 ug/m3 |
| **PM10** | Coarse particle pollution | 0--54 ug/m3 | 54--154 ug/m3 | >154 ug/m3 |

PM2.5 is the most important health metric. Common sources of high PM2.5 include cooking, candles, wildfires, traffic, and construction dust.

## Project Structure

```
airqualitymonitor/
|-- include/
|   |-- secrets.h.example   # Template for WiFi credentials
|   |-- secrets.h            # Your actual credentials (gitignored)
|   +-- README               # PlatformIO include directory docs
|-- lib/
|   +-- README               # PlatformIO library directory docs
|-- src/
|   +-- main.cpp             # All the code (heavily commented)
|-- test/
|   +-- README               # PlatformIO test directory docs
|-- .gitignore
+-- platformio.ini           # Build config and library dependencies
```

## Troubleshooting

### "WiFi connect failed, rebooting..."

- Double-check the SSID and password in `include/secrets.h` (they're case-sensitive).
- Make sure you're using the 2.4 GHz network, not 5 GHz.
- Move the ESP32 closer to your router.
- Check that your router isn't blocking new devices (MAC filtering).

### DHT22 shows "sensor error"

- Check wiring: VCC to 3.3V, DATA to GPIO 4, GND to GND.
- If using a bare 4-pin sensor, make sure you have the 10K pull-up resistor between DATA and VCC.
- Try a different GPIO pin (update `DHTPIN` in `main.cpp` to match).
- The DHT22 occasionally returns bad reads -- if the error is intermittent, it's normal. The code keeps the last good reading.

### PMS5003 stuck on "warming up"

- Check wiring: VCC to 5V (VIN), not 3.3V. GND to GND. TX to GPIO 16, RX to GPIO 17.
- Make sure TX/RX aren't swapped (the PMS5003 TX goes to the ESP32 RX2).
- The PMS5003 fan should spin visibly/audibly when powered. If it doesn't, it's not getting enough power.
- Try a different USB power source -- some laptop USB ports can't supply enough current for both the ESP32 and the PMS5003 fan.

### Can't find the ESP32's IP address

- Open the Serial Monitor at 115200 baud and press the reset button on the ESP32.
- If you see garbled text, the baud rate is wrong -- make sure it's set to 115200.
- Check your router's admin page for connected devices -- look for "espressif" or "ESP32".

### Page loads but shows stale data

- The page auto-refreshes every 30 seconds. If values seem frozen, the sensors may be disconnected. Check the Serial Monitor for error messages.
- Try power-cycling the ESP32 (unplug and replug USB).

## Power Consumption

### Component Breakdown

| Component | Active | Sleep/Idle | Notes |
|-----------|--------|------------|-------|
| **ESP32 (WiFi active)** | ~160--240 mA | ~10 mA (deep sleep) | WiFi is always on in this project |
| **DHT22** | ~1.5 mA (during read) | ~50 uA (idle) | Reads once every 2 seconds |
| **PMS5003** | ~100 mA | ~2 mA (sleep mode) | Fan + laser are the big draw |
| **Total (PMS awake)** | ~260--340 mA | | |
| **Total (PMS sleeping)** | ~160--240 mA | | |

### Duty Cycle Optimization

The PMS5003's fan and laser are the biggest power consumers after the ESP32 itself. Running them 24/7 wastes power and wears out the fan faster. The code implements a duty cycle:

1. **Wake** the PMS5003 -- fan spins up, laser turns on
2. **Wait 30 seconds** for the airflow to stabilize (datasheet recommendation)
3. **Read** the PM values
4. **Sleep** the PMS5003 for 90 seconds -- fan and laser turn off

This gives a 2-minute measurement cycle with the sensor active only 25% of the time. The average PMS5003 draw drops from ~100 mA to ~25 mA.

### Estimated Average Draw

With the default duty cycle (30 s wake / 90 s sleep):

```
ESP32 (WiFi on):     ~200 mA  (always on)
DHT22:                 ~1 mA  (negligible)
PMS5003 (25% duty):  ~25 mA  (averaged over the cycle)
                     --------
Total:               ~226 mA average
```

### Run Time Estimates

| Power Source | Capacity | Estimated Run Time |
|--------------|----------|-------------------|
| USB port (500 mA) | Continuous | Indefinite (wall-powered) |
| 10,000 mAh power bank | 10,000 mAh | ~40--44 hours |
| 5,000 mAh power bank | 5,000 mAh | ~20--22 hours |
| 18650 Li-ion cell (1S) | ~3,000 mAh | ~13 hours (needs 3.3V regulator) |

> **Note:** These are rough estimates. Real-world run time depends on WiFi signal strength (weaker signal = more TX power), how often browsers request the page, and power bank efficiency (typically 85--90% for the DC-DC converter).

### Tuning the Duty Cycle

You can adjust the duty cycle constants in `main.cpp` to trade freshness for power:

| Setting | Default | Effect of Increasing |
|---------|---------|---------------------|
| `PMS_WAKE_MS` | 30000 (30 s) | Longer warm-up = more stable first reading, but more power |
| `PMS_SLEEP_MS` | 90000 (90 s) | Longer sleep = less power, but staler PM data |

**Examples:**

- **Maximum freshness:** `PMS_WAKE_MS = 30000`, `PMS_SLEEP_MS = 30000` (50% duty, ~1 min cycle)
- **Maximum power savings:** `PMS_WAKE_MS = 30000`, `PMS_SLEEP_MS = 270000` (10% duty, ~5 min cycle)
- **Default balance:** `PMS_WAKE_MS = 30000`, `PMS_SLEEP_MS = 90000` (25% duty, ~2 min cycle)

## Future Ideas

- **mDNS**: Access the dashboard at `http://airquality.local` instead of an IP address
- **JSON API**: Add a `/api/data` endpoint for integration with Home Assistant, Grafana, or other tools
- **OTA updates**: Flash new firmware over WiFi instead of USB
- **AQI calculation**: Convert raw PM2.5 to the EPA Air Quality Index (a 0--500 scale)
- **Data logging**: Store readings on the ESP32's flash (SPIFFS/LittleFS) or push to InfluxDB/MQTT
- **Status LED**: Wire up an RGB LED that changes color based on air quality (green/yellow/red)

## License

This is a personal hobby project. Use it however you like.