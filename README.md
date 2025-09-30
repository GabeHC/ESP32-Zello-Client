# ESP32 Zello Client

An ESP32-based hardware client for Zello Push-to-Talk service that can connect to Zello channels and play incoming audio through a speaker.

## Features

- Connects to Zello channels via WebSocket API
- Decodes Opus audio streams in real-time
- Responsive web dashboard for device control
- Volume and speaker controls via hardware buttons or web interface
- Audio enhancement options with multiple profiles
- OTA (Over-The-Air) firmware updates
- Web-based configuration for WiFi and Zello settings
- UTF-8 compatible for international usernames and channel names

## Recent Updates

### Version 1.0.4 (September 2025)
- Fixed stack overflow issues in `audioTxTask` and `loop` functions
- Improved token refresh functionality and SPIFFS writes
- Added stack monitoring for debugging high stack usage
- Enhanced WebSocket reconnection logic
- Updated debug logs for better diagnostics

### Version 1.0.3 (April 2025)
- Added web-based configuration for Zello account settings
- Added web-based configuration for WiFi settings
- Improved UTF-8 support for international character sets
- Fixed issue with audio enhancement toggle
- Added detailed status information to the dashboard
- Added auto-reconnect functionality for WebSocket

## Hardware Requirements

-   ESP32-A1S Development Board (or similar ESP32 with AC101/ES8388 audio codec)
-   Buttons connected to GPIOs:
    -   Play/Mute: GPIO 23
    -   Volume Up: GPIO 18
    -   Volume Down: GPIO 5
-   Speaker/Headphones connected to the audio output jack.

## Setup Instructions

1.  **Clone Repository:** `git clone https://github.com/GabeHC/ESP32-Zello-Client.git`
2.  **PlatformIO:** Open the cloned folder in VS Code with the PlatformIO extension installed.
3.  **Configure SPIFFS:**
    *   Create a `data` directory in the project root (`Zello-Client/data`).
    *   Inside `data`, create the following files:
        *   `wifi_credentials.ini`:
            ```ini
            ssid=YOUR_WIFI_SSID
            password=YOUR_WIFI_PASSWORD
            ```
        *   `zello-api.key`: Paste your Zello Channel API key into this file (just the key, no extra text).
        *   `zello-io.crt`: Place the Zello CA certificate file here (downloadable or provided).
    *   Use the PlatformIO "Upload Filesystem Image" task to upload the contents of the `data` directory to SPIFFS.
4.  **Build and Upload:** Use PlatformIO to build and upload the firmware to the ESP32 via USB for the first time.
5.  **Subsequent Updates:** Access the web interface at `http://<ESP32_IP_ADDRESS>` (port 80) to upload new firmware via OTA.

## Audio Packet Format (Zello WebSocket)
   Byte 0: Packet type (0x01 for audio)
   Bytes 1-4: Stream ID (32-bit big-endian)
   Bytes 5-8: Packet ID (32-bit big-endian)
   Bytes 9+: OPUS encoded audio data
   
## Project Vision

🎯 **Final Goal**: Standalone Zello Radio Gateway
- Compact handheld form factor
- Color LCD display for channel/status
- Touch wheel interface for navigation
- LiPo battery powered
- USB-C charging/data interface
- Custom designed enclosure
- No phone/computer required

## Current Status

### ✅ Working:
- WebSocket connection to Zello server
- HTTPS certificate validation
- WiFi credentials management via SPIFFS
- OTA (Over The Air) updates
- Basic audio setup (I2S, AC101 codec)
- Volume control buttons

### ❌ In Progress:
- OPUS audio playback (Current Issue)
  - WebSocket packets received
  - OPUS decoding attempted
  - No audio output yet
- Audio transmission (Not Started)

## Hardware

### Current Development Hardware:
- ESP32-A1S Audio Kit
- Built-in AC101 codec
- Onboard speaker outputs
- Volume control buttons
- 3.5mm headphone jack

### Planned Final Hardware:
- ESP32-S3 main board
- 2.8" Color LCD Display
- Capacitive wheel controller
- 2000mAh LiPo battery
- USB-C port for charging/data
- Stereo DAC and amplifier
- Custom designed enclosure
- Built-in speaker
- 3.5mm TRRS audio jack for:
  - Stereo audio output
  - Mono microphone input
  - PTT button support

## Connectivity

### WiFi
- Primary connection for Zello service
- OTA firmware updates
- Web configuration interface

### Bluetooth
- Optional wireless audio output
- External headset/speaker support
- A2DP sink and source profiles
- HFP for headset support

## References & Resources

📚 **Documentation**
-   [ESP32-A1S Audio Kit Schematic](https://docs.ai-thinker.com/_media/esp32/docs/esp32-a1s_v2.3_specification.pdf)
-   [AC101 Codec Driver (Reference)](https://github.com/Yveaux/AC101)
-   [arduino-audio-tools Library](https://github.com/pschatzmann/arduino-audio-tools) (Used for AudioBoardStream)
-   [Opus Audio Codec](https://opus-codec.org/docs/)
-   [Zello Channel API Documentation](https://github.com/zelloptt/zello-channel-api/blob/master/API.md)

## Legal Notice

⚠️ **Disclaimer**:
-   This is a DIY educational project for personal use only.
-   Not affiliated with Zello Inc.
-   Use responsibly and ensure compliance with Zello's terms of service.

## License

[MIT License](LICENSE)
