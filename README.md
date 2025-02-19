# Zello WebSocket Client for ESP32

An ESP32-based client for Zello using WebSocket protocol and OPUS audio decoder.

## Features

- WebSocket connection to Zello servers
- Binary audio packet handling
- Custom OPUS decoder integration
- I2S audio output via AC101 codec
- OTA updates via web interface
- Secure connection with SSL/TLS
- Channel status monitoring
- Stream statistics

## Hardware Requirements

- ESP32-A1S or compatible board
- AC101 audio codec
- I2S connection
- Optional: External amplifier (GPIO 21)

## Pin Configuration
```cpp
#define IIS_SCLK 27    // I2S Serial Clock
#define IIS_LCLK 26    // I2S Word Select
#define IIS_DSIN 25    // I2S Data

#define IIC_CLK 32     // I2C Clock for AC101
#define IIC_DATA 33    // I2C Data for AC101

#define GPIO_PA_EN 21  // Power Amplifier Enable
```

## Binary Protocol Implementation

### Audio Packet Format
```
Byte 0:    Packet type (0x01 for audio)
Bytes 1-4: Stream ID (32-bit big-endian)
Bytes 5-8: Packet ID (32-bit big-endian)
Bytes 9+:  OPUS encoded audio data
```

### Debug Output Example
```
=== Raw Binary Packet 1 of 5 ===
Raw Length: 58 bytes
Raw Data: 01 00 00 0B 92 00 00 00 00 | 0A 07 0B...

Audio Packet 0:
Stream ID: 2962
Packet ID: 0
Opus Length: 49
```

## Implementation Details

1. **WebSocket Connection**
   - Secure WSS connection to zello.io
   - Custom certificate validation
   - Automatic reconnection handling

2. **Audio Processing**
   - OPUS packet buffering
   - Fixed-size buffer (8KB)
   - I2S output configuration
   - AC101 codec initialization

3. **Debug Features**
   - Detailed packet inspection
   - Stream statistics
   - Buffer state monitoring
   - Memory usage tracking

## Current Status

- [x] WebSocket connection working
- [x] Binary packet parsing implemented
- [x] Buffer management working
- [x] Stream start/stop handling
- [x] Channel status reporting
- [ ] OPUS decoding optimization needed
- [ ] Audio playback quality improvement needed

## Next Steps

1. Switch to direct libopus implementation
2. Improve buffer management
3. Add error recovery mechanisms
4. Implement volume control
5. Add channel switching capability