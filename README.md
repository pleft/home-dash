# Home Dashboard - RuuviTag ESP32 Monitor

ESP32-based IoT dashboard for monitoring RuuviTag environmental sensors with TFT display and web interface.

## Features

- **Real-time BLE scanning** for RuuviTag sensors (temperature, humidity, pressure, battery)
- **TFT display** with card-based sensor layout
- **Web dashboard** with live sensor cards (charts removed)
- **WiFi + AP fallback** mode for easy setup
- **History storage and charts removed** to simplify the system

## Hardware Requirements

- ESP32 development board
- TFT display (compatible with TFT_eSPI)
- RuuviTag sensors

## Quick Start

### Windows WSL Users
For Windows users with WSL, see [README_WSL.md](README_WSL.md) for complete setup instructions including USB device sharing and PowerShell commands. Those instructions also show the exact commands to compile with `HAS_TFT=0` and the `no_ota` partition scheme.

### Standard Setup

#### 1. Install Dependencies
```bash
# Install ESP32 board package
arduino-cli core update-index
arduino-cli core install esp32:esp32

# Install required libraries
arduino-cli lib install "NimBLE-Arduino" "TFT_eSPI"
```

#### 2. Build and Upload
```bash
# Compile firmware (no_ota partition scheme)
arduino-cli compile \
  --fqbn esp32:esp32:esp32:PartitionScheme=no_ota \
  --build-path build \
  home-dash.ino

# Upload firmware (replace your serial port)
arduino-cli upload -p /dev/ttyUSB0 \
  --fqbn esp32:esp32:esp32:PartitionScheme=no_ota \
  --build-path build \
  home-dash.ino
```

#### 3. Upload Web Files (LittleFS)

```bash
# Create LittleFS image (adjust size to your partition map if needed)
mklittlefs -c data -b 4096 -s 0x1E0000 data.littlefs.bin

# Upload LittleFS image (replace port if needed)
esptool --chip esp32 --port /dev/ttyUSB0 --baud 921600 write_flash 0x210000 data.littlefs.bin
```

Alternatively, use the web uploader at `/upload`.

#### 4. Access Dashboard
- **WiFi Mode**: `http://home.local/` or ESP32 IP
- **AP Mode**: `http://192.168.4.1/` (connect to `RuuviSetup-XXXX` network)

## Configuration

### WiFi Setup
Create `/wifi.properties`:
```
ssid=YourWiFiNetwork
password=YourWiFiPassword
```

### Sensor Names (Optional)
Create `/names.csv`:
```
MAC,Name
AA:BB:CC:DD:EE:FF,Living Room
11:22:33:44:55:66,Bedroom
```

## Build Options

**TFT Display (default enabled)**
```bash
arduino-cli compile \
  --fqbn esp32:esp32:esp32:PartitionScheme=no_ota \
  --build-path build \
  home-dash.ino
```

**Headless (No Display)**
```bash
arduino-cli compile \
  --fqbn esp32:esp32:esp32:PartitionScheme=no_ota \
  --build-property compiler.cpp.extra_flags="-DHAS_TFT=0" \
  --build-path build \
  home-dash.ino
```

## Troubleshooting

**Upload Issues**
- Check serial port: `arduino-cli board list`
- Put ESP32 in boot mode (hold BOOT, press RESET, release RESET, release BOOT)
- Try a different USB cable/port

**WiFi Issues**
- Use 2.4GHz network (ESP32 doesn't support 5GHz)
- Check credentials in `/wifi.properties`
- Use AP mode fallback: connect to `RuuviSetup-XXXX`

**Display Issues**
- Verify TFT_eSPI library configuration (see `display/README.md`)
- Check wiring connections

**Web Interface Issues**
- Verify LittleFS files are uploaded
- Check ESP32 IP address
- Try accessing via IP instead of mDNS

## Performance

- **Program Storage**: ~1.41 MB (67% of 2MB app partition)
- **Dynamic Memory**: ~60 KB (18% of 320KB)
- **Update Frequency**: Sensor updates every 200ms (throttle), display every 500ms

## Project Structure

```
home-dash/
├── home-dash.ino          # Main Arduino sketch
├── data/                  # Web interface files
│   ├── index.html        # Dashboard page
│   ├── app.js            # JavaScript app
│   ├── styles.css        # CSS styles
│   ├── names.csv         # Sensor names
│   └── wifi.properties   # WiFi credentials
├── display/              # TFT display configuration
│   ├── User_Setup.h      # TFT_eSPI library config
│   └── README.md         # Display setup instructions
├── README.md             # Main documentation
└── README_WSL.md         # WSL-specific instructions
```

## API Endpoints

- `GET /data`  - Real-time sensor data
- `GET /health` - System metrics
- `GET /heap`  - Heap and fragmentation metrics (free, min, largest, frag)
- `GET /config` - WiFi configuration
- `GET /upload` - File upload interface

---

**Version**: 0.1 | **Last Updated**: 2025