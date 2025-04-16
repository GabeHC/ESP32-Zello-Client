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

### Version 1.0.3 (April 2025)
- Added web-based configuration for Zello account settings
- Added web-based configuration for WiFi settings
- Improved UTF-8 support for international character sets
- Fixed issue with audio enhancement toggle
- Added detailed status information to the dashboard
- Added auto-reconnect functionality for WebSocket

## Hardware Requirements

- ESP32 development board
- AC101 audio codec or compatible I2S DAC
- Speaker with amplifier
- Physical buttons for control (optional)

## Configuration

### Web Interface

The device provides a web interface accessible via the IP address shown during boot. The web interface allows you to:

1. View system status and diagnostics
2. Control volume and speaker
3. Toggle audio enhancement
4. Configure WiFi credentials
5. Configure Zello account and channel settings
6. Perform OTA firmware updates

### WiFi Configuration

Access the WiFi configuration page by clicking the "WiFi Settings" button on the dashboard. This allows you to set:
- WiFi SSID
- WiFi password

The device will automatically reboot after saving WiFi settings to apply the changes.

### Zello Configuration

Access the Zello configuration page by clicking the "Zello Settings" button on the dashboard. This allows you to set:
- Zello username
- Zello password
- Zello channel name
- Zello API token

The device will automatically reconnect to Zello after saving these settings.

## Installation

1. Clone this repository
2. Configure your `platformio.ini` with the appropriate board and settings
3. Create a `wifi_credentials.ini` file in the `data` folder with your credentials
4. Create a `zello-api.key` file in the `data` folder with your Zello API token
5. Upload the code and file system to your ESP32

## File Structure

The following files are stored in the ESP32's SPIFFS file system:
- `/wifi_credentials.ini` - Contains WiFi and Zello user credentials
- `/zello-api.key` - Contains the Zello API token
- `/zello-io.crt` - SSL certificate for secure WebSocket connection

## License

[MIT License](LICENSE)
