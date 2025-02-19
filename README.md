# ESP32 Zello Audio Client (Work In Progress)

An experimental WebSocket client for Zello running on ESP32-A1S audio development board, with the goal of creating a standalone portable Zello radio gateway. 
**⚠️ This project is under active development and not yet fully functional.**

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

## Setup Instructions

1. Clone repository
2. Run `setup-project.bat`
3. Configure WiFi credentials in `wifi_config.txt`
4. First upload via USB
5. Subsequent updates via OTA

## References & Resources

📚 **Documentation**
- [ESP32-A1S Audio Kit Schematic](https://docs.ai-thinker.com/_media/esp32/docs/esp32-a1s_v2.3_specification.pdf)
- [AC101 Codec Driver](https://github.com/Yveaux/AC101)
- [ESP8266Audio Library](https://github.com/earlephilhower/ESP8266Audio)
- [Opus Audio Codec](https://opus-codec.org/docs/)
- [Zello SDK Documentation](https://github.com/zelloptt/zello-channel-api/blob/master/API.md)

## Legal Notice

⚠️ **Disclaimer**:
- This is a DIY educational project for personal use only
- Not affiliated with Zello Inc.
- Project designed for learning purposes
- Zello® is a trademark of Zello Inc.

## License

MIT License - See LICENSE file
