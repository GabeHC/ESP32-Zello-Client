# ESP32 Zello Audio Client (Work In Progress)

This project implements a Zello client on an ESP32 (specifically tested with ESP32-A1S board with AC101 codec) using the Zello Channel API over WebSockets.

## Status / Features

-   **WiFi Connection:** Working
-   **WebSocket Connection (Secure):** Working
-   **Zello Authentication:** Working
-   **Stream Start/Stop Handling:** Working
-   **Opus Decoding:** Working
-   **Audio Playback (AC101/ES8388): Working**
-   **Button Input (Volume Up/Down): Working**
-   **OTA Updates (Web Interface): Working**
-   **SPIFFS for Configuration:** Working (WiFi credentials, Zello API key, CA Cert)
-   **NTP Time Sync:** Working

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
- OPUS audio playback 

### ❌ In Progress:
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

MIT License - See LICENSE file
