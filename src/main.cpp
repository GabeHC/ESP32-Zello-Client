#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoWebsockets.h>
#include <FS.h>
#include <SPIFFS.h>
#include <WebServer.h> // Include the WebServer library
#include <Update.h>  // Add this for OTA functionality
#include <mbedtls/base64.h> // Add this include for base64 decoding
#include "opus.h"
#include "opus_defines.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"

#include <AudioTools.h> 
#include <AudioTools/AudioLibs/AudioBoardStream.h> // *** Add the 'AudioTools' directory level ***

#include <WiFiUdp.h>
#include <NTPClient.h>

#define OPUS_BUFFER_SIZE 8192
#define MIN_BUFFER_SIZE 1024

#define IIS_SCLK 27
#define IIS_LCLK 26
#define IIS_DSIN 25

#define IIC_CLK 32
#define IIC_DATA 33

#define GPIO_PA_EN GPIO_NUM_21
#define GPIO_SEL_PA_EN GPIO_SEL_21

#define PIN_PLAY (23)      // KEY 4
#define PIN_VOL_UP (18)    // KEY 5
#define PIN_VOL_DOWN (5)   // KEY 6

#define DETAILED_PACKET_COUNT 5 // Add this declaration

#define FRAME_SIZE 960  // For 48kHz, 20ms frame
#define CHANNELS 1
#define MAX_FRAME_SIZE 6*960
#define MAX_PACKET_SIZE 3828

// Remove these AC101 defines
// #define AC101_DAC_MIXER_CTRL    0x23
// #define AC101_L_DAC_MIXER_SRC   0x21
// #define AC101_R_DAC_MIXER_SRC   0x22
// #define AC101_PATH_CTRL         0x02
// #define AC101_PWR_CTRL1        0x03
// #define AC101_PWR_CTRL2        0x04

// Remove I2C Timeout if not used elsewhere
// #define I2C_TIMEOUT 1000  

static uint8_t volume = 40;        // Start at ~63% volume (0-63 range)
const uint8_t volume_step = 5;     // Larger steps for quicker adjustment
float initialVolumeFloat = 0.63f; // Store the initial float volume set in setup

using namespace websockets;

// Remove the const char* updateHTML = R"( ... )"; definition

// Forward declarations and structures (add at the top after includes)
// Remove unused forward declarations
// class AC101;
// class AudioOutputI2S;
class WebServer; // Keep this one

// Structure definitions
struct OpusConfig {
    uint16_t sampleRate;
    uint8_t framesPerPacket;  // Fix: removed typo uint88_t
    uint8_t frameSizeMs;
};

struct OpusPacket {
    const uint8_t* data;
    size_t length;
    uint8_t frameCount;    // Should be 6
    uint8_t frameDuration; // Should be 20ms
};

// Global variables
WebServer server(8080);
AudioBoardStream audioKit(AudioKitAC101); 

// Add global variables for dynamic buffer

// Add these global variables near the top with other declarations
unsigned long streamStartTime = 0;
unsigned long streamDuration = 0;
size_t totalBytesReceived = 0;
int totalPacketsReceived = 0;

// Add these with other global variables
bool isValidAudioStream = false;
int binaryPacketCount = 0;

// Websocket and Zello-related variables
WebsocketsClient client;
String ssid;
String password;
String token;
const char* websocket_server = "wss://zello.io/ws"; // Use wss:// for secure WebSocket

// Add these global variables
OpusDecoder* decoder = nullptr;
int16_t* pcmBuffer = nullptr;
size_t pcmBufferSize = 0;

// NTP client variables
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 0, 60000); // UTC, update every 60s

// Function declarations
void onMessageCallback(WebsocketsMessage message);
void onEventsCallback(WebsocketsEvent event, String data);
void readCredentials();
void setupOTAWebServer();  // Forward declaration
OpusPacket findNextOpusPacket(const uint8_t* data, size_t len);
bool initOpusDecoder(int sampleRate); // Add this forward declaration
void enableSpeakerAmp(bool enable); // *** ADD THIS FORWARD DECLARATION ***

// Update the validateOpusPacket function
bool validateOpusPacket(const uint8_t* data, size_t len) {
    if (len < 2) return false;
    
    uint8_t toc = data[0];
    uint8_t config = toc >> 3;        // First 5 bits
    uint8_t s = (toc >> 2) & 0x1;     // 1 bit
    uint8_t c = toc & 0x3;            // Last 2 bits
    
    // Debug output for first few packets
    if (binaryPacketCount < DETAILED_PACKET_COUNT) {
        Serial.printf("\nValidating OPUS packet:\n");
        Serial.printf("- TOC: 0x%02X\n", toc);
        Serial.printf("- Config: %d (mode=%s)\n", config, 
            config <= 4 ? "SILK-only" : 
            config <= 7 ? "Hybrid" : "CELT-only");
        Serial.printf("- VBR flag: %d\n", s);
        Serial.printf("- Channels: %d\n", c + 1);
        Serial.printf("- Length: %d bytes\n", len);
    }
    
    // Less strict validation for Zello packets
    if (len < 8) return false;        // Too short to be valid
    if (config > 31) return false;    // Invalid configuration
    
    // Don't validate channel count as it appears to be incorrect in header
    // Instead, we'll force mono output in the decoder
    
    return true;
}
// Add this debug function
void debugOpusFrame(const uint8_t* data, size_t len, int frameNum) {
    Serial.printf("\nOPUS Frame %d Analysis:\n", frameNum);
    if (len < 2) {
        Serial.println("Frame too short!");
        return;
    }

    uint8_t toc = data[0];
    uint8_t config = toc >> 3;
    uint8_t s = (toc >> 2) & 0x1;
    uint8_t c = toc & 0x3;
    
    Serial.printf("TOC: 0x%02X\n", toc);
    Serial.printf("Config: %d\n", config);
    Serial.printf("s (VBR flag): %d\n", s);
    Serial.printf("c (channels): %d\n", c);
    
    // Print first 16 bytes
    Serial.print("Data: ");
    for (int i = 0; i < min(16, (int)len); i++) {
        Serial.printf("%02X ", data[i]);
    }
    Serial.println();
}

// Update findNextOpusPacket function
OpusPacket findNextOpusPacket(const uint8_t* data, size_t len) {
    OpusPacket packet = {nullptr, 0, 1, 20};  // Changed frameCount to 1
    
    if (len < 10) {  // 9 byte header + at least 1 byte data
        Serial.println("Packet too short for header");
        return packet;
    }
    
    // Validate OPUS data
    if (!validateOpusPacket(&data[9], len - 9)) {
        Serial.println("Invalid OPUS data");
        return packet;
    }
    
    // Extract OPUS frame
    packet.data = &data[9];
    packet.length = len - 9;
    
    if (binaryPacketCount < DETAILED_PACKET_COUNT) {
        Serial.printf("Valid OPUS frame found: size=%d bytes\n", packet.length);
    }
    
    return packet;
}

// New function to print hex and char representation of data
void printHexAndChar(const uint8_t* data, size_t len, const char* label) {
    Serial.printf("\n=== %s (%d bytes) ===\n", label, len);
    Serial.print("HEX: ");
    for (size_t i = 0; i < len; i++) {
        Serial.printf("%02X ", data[i]);
    }
    Serial.print("\nCHR: ");
    for (size_t i = 0; i < len; i++) {
        if (data[i] >= 32 && data[i] <= 126) {
            Serial.printf(" %c ", data[i]);
        } else {
            Serial.print(" . ");
        }
    }
    Serial.println("\n==================\n");
}
// Modify onMessageCallback for binary messages
void onMessageCallback(WebsocketsMessage message) {
    if (message.isBinary()) {
        // *** DEBUG: Confirm binary message received ***
        // Serial.println("DEBUG: Received Binary Message"); // Can be noisy, enable if needed

        size_t msgLen = message.length();
        const uint8_t* rawData = (const uint8_t*)message.c_str();

        // Print first packet details (already exists, good)
        if (binaryPacketCount == 0) {
            Serial.println("\nFirst packet details:");
            Serial.printf("Total length: %d bytes\n", msgLen);
            Serial.printf("Packet type: 0x%02X\n", rawData[0]);
            Serial.printf("OPUS data length: %d bytes\n", msgLen - 9);
        }

        // Validate packet type (already exists, good)
        if (rawData[0] != 0x01) {
            Serial.printf("Invalid packet type: 0x%02X\n", rawData[0]);
            return;
        }

        // Extract OPUS data (skip 9-byte header)
        const uint8_t* opusData = rawData + 9;
        size_t opusLen = msgLen - 9;

        // *** DEBUG: Log OPUS data length ***
        // Serial.printf("DEBUG: Extracted OPUS data length: %d\n", opusLen); // Can be noisy

        // Print OPUS packet details for first few packets (already exists, good)
        if (binaryPacketCount < DETAILED_PACKET_COUNT) {
            Serial.printf("\nOPUS Packet %d:\n", binaryPacketCount);
            Serial.printf("- Length: %d bytes\n", opusLen);
            Serial.printf("- First 8 bytes: ");
            for (int i = 0; i < min(8, (int)opusLen); i++) {
                Serial.printf("%02X ", opusData[i]);
            }
            Serial.println();
        }

        // Decode OPUS directly
        if (decoder && pcmBuffer) { // *** DEBUG: Check if decoder and buffer are valid ***
            // Always show critical errors regardless of packet count (already exists, good)
            if (opusLen < 2) {
                Serial.println("OPUS packet too small");
                return;
            }
            if (opusLen > MAX_PACKET_SIZE) {
                Serial.printf("OPUS packet too large: %d > %d\n", opusLen, MAX_PACKET_SIZE);
                return;
            }

            // *** DEBUG: Log before calling opus_decode ***
            // Serial.printf("DEBUG: Calling opus_decode (opusLen=%d, max_frame_size=%d)\n", opusLen, MAX_FRAME_SIZE);

            int samples_decoded = opus_decode(decoder, opusData, opusLen, pcmBuffer, MAX_FRAME_SIZE, 0);

            // *** DEBUG: Log after calling opus_decode ***
            // Serial.printf("DEBUG: opus_decode returned: %d\n", samples_decoded);

            // Always show decode errors (already exists, good)
            if (samples_decoded < 0) {
                Serial.printf("OPUS decode error %d: %s\n", samples_decoded, opus_strerror(samples_decoded));
                return; // Don't try to play bad data
            }

            // Check if we got valid samples
            if (samples_decoded > 0) {
                // *** DEBUG: Log number of samples decoded ***
                // Serial.printf("DEBUG: Decoded %d mono samples\n", samples_decoded);

                // *** Allocate stereoBuffer dynamically on the heap ***
                size_t stereoBufferSize = samples_decoded * 2;
                int16_t* stereoBuffer = (int16_t*)malloc(stereoBufferSize * sizeof(int16_t));

                if (!stereoBuffer) {
                    Serial.println("ERROR: Failed to allocate stereoBuffer!");
                    // Skip processing this packet if allocation fails
                    // Increment counters before returning
                    totalBytesReceived += opusLen; 
                    totalPacketsReceived++;
                    binaryPacketCount++; 
                    return; 
                }

                // *** DEBUG: Log stereo buffer allocation ***
                // Serial.printf("DEBUG: Allocated stereoBuffer (%d samples, %d bytes) at %p\n", 
                //               stereoBufferSize, stereoBufferSize * sizeof(int16_t), stereoBuffer);

                // Prepare stereo buffer
                for (int i = 0; i < samples_decoded; i++) {
                    stereoBuffer[i * 2]     = pcmBuffer[i];
                    stereoBuffer[i * 2 + 1] = pcmBuffer[i];
                }

                // Log current volume before writing (already exists, good)
                Serial.printf("Volume before write: %d (%.2f)\n", volume, volume / 63.0f);

                // Write data using AudioKit stream
                size_t bytes_to_write = stereoBufferSize * sizeof(int16_t); // Use stereoBufferSize

                // *** DEBUG: Log before calling audioKit.write ***
                // Serial.printf("DEBUG: Calling audioKit.write (bytes_to_write=%d)\n", bytes_to_write);

                size_t bytes_written = audioKit.write((uint8_t*)stereoBuffer, bytes_to_write);

                // *** DEBUG: Log after calling audioKit.write ***
                // Serial.printf("DEBUG: audioKit.write returned: %d\n", bytes_written);

                // *** Free the dynamically allocated buffer ***
                free(stereoBuffer); 
                // Serial.println("DEBUG: Freed stereoBuffer"); // Uncomment if needed

                // Log consumption status (already exists, good)
                bool consumed_fully = (bytes_written == bytes_to_write);
                 if (!consumed_fully || binaryPacketCount == 0 || binaryPacketCount % 100 == 0) {
                    Serial.printf("AudioKit Write: packet=%d, samples=%d, bytes=%d/%d, consumed=%s\n",
                                  binaryPacketCount, samples_decoded, bytes_written, bytes_to_write,
                                  consumed_fully ? "OK" : "PARTIAL/FAIL");
                }
                // ... rest of consumeFailCount logic ...

            } else { // samples_decoded <= 0
                 // ... existing handling ...
            }
        } else { // decoder or pcmBuffer NULL
            // ... existing handling ...
        }
        // Increment counters (Make sure these are placed correctly)
        totalBytesReceived += opusLen;
        totalPacketsReceived++;
        binaryPacketCount++; // Increment after processing
    } else { // Text message handling
        String msg = message.data();

        if (msg.indexOf("\"command\":\"on_stream_start\"") >= 0) {
            Serial.println("\n=== Stream Start Message ===");
            Serial.println(msg);
            Serial.println("===========================\n");

            // *** DEBUG: Check if we enter the header parsing block ***
            Serial.println("DEBUG: Attempting to find codec_header...");

            int headerStart = msg.indexOf("\"codec_header\":\"");
            if (headerStart >= 0) {
                // *** DEBUG: Found header start index ***
                Serial.printf("DEBUG: Found codec_header at index %d\n", headerStart);
                headerStart += 16; // Correct offset for "\"codec_header\":\""
                int headerEnd = msg.indexOf("\"", headerStart);
                if (headerEnd > headerStart) {
                    // *** DEBUG: Found header end index ***
                    Serial.printf("DEBUG: Found header end at index %d\n", headerEnd);
                    String codecHeader = msg.substring(headerStart, headerEnd);

                    // *** DEBUG: Print the extracted header ***
                    Serial.printf("DEBUG: Extracted Codec Header: [%s]\n", codecHeader.c_str());

                    // Parse base64 codec header
                    size_t decodedLen = 0; // Initialize
                    uint8_t decoded[4];
                    // *** Check the return value of base64 decode ***
                    int decode_ret = mbedtls_base64_decode(decoded, 4, &decodedLen,
                        (const uint8_t*)codecHeader.c_str(), codecHeader.length()); // Cast to const uint8_t*

                    // *** DEBUG: Print decode result and length ***
                    Serial.printf("DEBUG: Base64 decode result: %d, decoded length: %d\n", decode_ret, decodedLen);

                    if (decode_ret == 0 && decodedLen == 4) { // *** Check decode_ret too ***
                        // *** DEBUG: Decode successful, length is 4 ***
                        Serial.println("DEBUG: Base64 decode successful (len=4). Parsing config...");

                        OpusConfig config;
                        config.sampleRate = decoded[0] | (decoded[1] << 8);
                        config.framesPerPacket = decoded[2];
                        config.frameSizeMs = decoded[3];

                        Serial.printf("Opus Config: %dHz, %d frames/packet, %dms/frame\n",
                            config.sampleRate, config.framesPerPacket, config.frameSizeMs);

                        // *** DEBUG: Attempting to reconfigure audio ***
                        Serial.println("DEBUG: Attempting audio reconfiguration...");
                        auto cfg = audioKit.defaultConfig(TX_MODE);
                        cfg.sample_rate = config.sampleRate; // Use sample rate from Zello
                        cfg.bits_per_sample = 16;
                        cfg.channels = 2; // AudioKit expects stereo config
                        if (!audioKit.begin(cfg)) {
                             Serial.println("WARNING: Failed to apply updated audio config via begin()!");
                        } else {
                             Serial.printf("Audio parameters updated (%dHz, 16bit, Stereo).\n", config.sampleRate);
                             delay(10); // Keep small delay after successful begin()

                             // *** UNCOMMENT AND SET lower volume for the stream ***
                             float streamVolume = 0.2f; // Start low (e.g., 20%)
                             Serial.printf("Setting stream volume to %.2f\n", streamVolume);
                             audioKit.setVolume(streamVolume);
                        }

                        // *** DEBUG: Attempting to initialize decoder ***
                        Serial.println("DEBUG: Attempting Opus decoder initialization...");
                        if (!initOpusDecoder(config.sampleRate)) {
                             Serial.println("Failed to initialize Opus decoder");
                             // *** DEBUG: Added return on failure ***
                             return;
                        }

                        // *** DEBUG: Attempting to enable amplifier ***
                        Serial.println("DEBUG: Attempting to enable amplifier...");
                        enableSpeakerAmp(true);
                    } else {
                        // *** DEBUG: Decode failed or wrong length ***
                        Serial.println("DEBUG: Base64 decode FAILED or length != 4.");
                    }
                } else {
                     // *** DEBUG: Header end not found ***
                     Serial.println("DEBUG: Could not find end quote for codec_header.");
                }
            } else {
                // *** DEBUG: Header start not found ***
                Serial.println("DEBUG: Could not find start of codec_header string.");
            }

            // Reset stream state (moved outside the header parsing logic)
            streamStartTime = millis();
            totalBytesReceived = 0;
            totalPacketsReceived = 0;
            binaryPacketCount = 0; // Reset binary count here
            isValidAudioStream = true; // Set valid stream flag here
        } else if (msg.indexOf("\"command\":\"on_stream_stop\"") >= 0) {
            Serial.println("\n=== Stream Stop Message ===");
            Serial.println(msg);
            Serial.println("===========================\n");
            streamDuration = millis() - streamStartTime;
            
            Serial.println("\n=== Stream Statistics ===");
            Serial.printf("Duration: %.2f seconds\n", streamDuration/1000.0);
            Serial.printf("Total packets: %d\n", totalPacketsReceived);
            Serial.printf("OPUS bytes: %d\n", totalBytesReceived);
            Serial.printf("Packet rate: %.1f packets/s\n", 
                        (totalPacketsReceived * 1000.0) / streamDuration);
            Serial.println("=====================\n");

            isValidAudioStream = false;
            if (decoder) {
                opus_decoder_destroy(decoder);
                decoder = nullptr;
            }
            if (pcmBuffer) {
                free(pcmBuffer);
                pcmBuffer = nullptr;
            }
            // *** Disable amplifier ***
            Serial.println("Disabling speaker amplifier for stream stop...");
            enableSpeakerAmp(false); 

            // *** Restore initial volume after stream stops ***
            Serial.printf("Restoring initial volume to %.2f\n", initialVolumeFloat);
            audioKit.setVolume(initialVolumeFloat); // Restore the volume set during setup

        } else if (msg.indexOf("\"command\":\"channel_status\"") >= 0) {
            Serial.println("\n=== Channel Status ===");
            // Extract channel name
            int channelStart = msg.indexOf("\"channel\":\"");
            if (channelStart >= 0) {
                channelStart += 11;
                int channelEnd = msg.indexOf("\"", channelStart);
                if (channelEnd > channelStart) {
                    String channel = msg.substring(channelStart, channelEnd);
                    Serial.printf("Connected to channel: %s\n", channel.c_str());
                }
            }
            Serial.println("===================\n");
        }
    }
}

void onEventsCallback(WebsocketsEvent event, String data) {
    if (event == WebsocketsEvent::ConnectionOpened) {
        Serial.println("Connection Opened");
        // Logon message
        char logon[1024];
        sprintf(logon, "{\"command\": \"logon\",\"seq\": 1,\"auth_token\": \"%s\",\"username\": \"bv5dj-r\",\"password\": \"gabpas\",\"channel\": \"ZELLO無線聯合網\"}", token.c_str());
        client.send(logon);
    } else if (event == WebsocketsEvent::ConnectionClosed) {
        Serial.println("Connection Closed");
    } else if (event == WebsocketsEvent::GotPing) {
        Serial.println("Got Ping");
        client.pong();
    } else if (event == WebsocketsEvent::GotPong) {
        Serial.println("Got Pong");
    }
}

void readCredentials() {
    // Log SPIFFS contents once during boot
    Serial.println("\n=== SPIFFS Files ===");
    File root = SPIFFS.open("/");
    File file = root.openNextFile();
    while(file) {
        Serial.printf("- %s (%d bytes)\n", file.name(), file.size());
        file = root.openNextFile();
    }
    Serial.println("===================\n");

    // Rest of the credential reading code
    File wifiFile = SPIFFS.open("/wifi_credentials.ini", "r");
    if (!wifiFile) {
        Serial.println("Failed to open wifi_credentials.ini");
        return;
    }
    while (wifiFile.available()) {
        String line = wifiFile.readStringUntil('\n');
        line.trim();
        int separatorIndex = line.indexOf('=');
        if (separatorIndex != -1) {
            String key = line.substring(0, separatorIndex);
            String value = line.substring(separatorIndex + 1);
            if (key == "ssid") {
                ssid = value;
                Serial.println("SSID: [" + ssid + "]");
            } else if (key == "password") {
                password = value;
            }
        }
    }
    wifiFile.close();
    File tokenFile = SPIFFS.open("/zello-api.key", "r");
    if (!tokenFile) {
        Serial.println("Failed to open zello-api.key");
        return;
    }
    token = tokenFile.readString();
    token.trim();
    tokenFile.close();
}

void setupOTAWebServer() {
    server.on("/", HTTP_GET, []() {
        Serial.println("[OTA] Index page requested");
        File file = SPIFFS.open("/ota_update.html", "r");
        if (!file) {
            Serial.println("Failed to open ota_update.html");
            server.send(500, "text/plain", "Failed to load update page");
            return;
        }
        server.streamFile(file, "text/html");
        file.close();
    });

    server.on("/debug", HTTP_GET, []() {
        String debug = "Debug Info:\n";
        debug += "Free Heap: " + String(ESP.getFreeHeap()) + "\n";
        debug += "Total Heap: " + String(ESP.getHeapSize()) + "\n";
        debug += "Flash Size: " + String(ESP.getFlashChipSize()) + "\n";
        debug += "Free Sketch Space: " + String(ESP.getFreeSketchSpace()) + "\n";
        debug += "WiFi RSSI: " + String(WiFi.RSSI()) + "\n";
        debug += "Uptime: " + String(millis()/1000) + "s\n";
        server.send(200, "text/plain", debug);
    });

    server.on("/update", HTTP_POST, []() {
        server.sendHeader("Connection", "close");
        server.send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
        delay(1000);
        ESP.restart();
    }, []() {
        HTTPUpload& upload = server.upload();
        if (upload.status == UPLOAD_FILE_START) {
            Serial.printf("Update: %s\n", upload.filename.c_str());
            if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
                Update.printError(Serial);
            }
        } else if (upload.status == UPLOAD_FILE_WRITE) {
            if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
                Update.printError(Serial);
            }
        } else if (upload.status == UPLOAD_FILE_END) {
            if (Update.end(true)) {
                Serial.printf("Update Success: %u\nRebooting...\n", upload.totalSize);
            } else {
                Update.printError(Serial);
            }
        }
    });

    server.begin();
}
// Minimal setup to fix the Wire reference
void setup() {
    Serial.begin(115200);
    delay(100);
    Serial.println("\n\n=== Booting Zello Client (using AudioBoardStream) ===");

    // --- STEP 1: Initialize AudioKit ---
    Serial.println("Initializing AudioBoardStream (relying on build flags)...");
    auto cfg = audioKit.defaultConfig(TX_MODE);
    cfg.sample_rate = 48000;
    cfg.channels = 2;
    cfg.bits_per_sample = 16;

    // *** Print the configuration WE ARE TRYING TO SET ***
    Serial.println("Attempting AudioKit Config:");
    Serial.printf("- Sample Rate: %d\n", cfg.sample_rate);
    Serial.printf("- Channels: %d\n", cfg.channels);
    Serial.printf("- Bits/Sample: %d\n", cfg.bits_per_sample);
    Serial.printf("- BCLK Pin: %d\n", cfg.pin_bck);
    Serial.printf("- WS Pin: %d\n", cfg.pin_ws);
    Serial.printf("- DOUT Pin: %d\n", cfg.pin_data);

    if (!audioKit.begin(cfg)) { // *** Call begin(cfg) ***
        Serial.println("AudioBoardStream initialization FAILED! Halting.");
        while(1) { delay(1000); }
    } else {
        Serial.println("AudioBoardStream initialized successfully.");
        
        // *** Store the intended initial volume ***
        initialVolumeFloat = volume / 63.0f; 
        audioKit.setVolume(initialVolumeFloat); // Set initial volume for later use
        Serial.printf("Initial volume set to %d (%.2f)\n", volume, initialVolumeFloat);
        Serial.println("Audio parameters set via config (48kHz, 16bit, Stereo).");

        // *** Play a startup tone ***
        Serial.println("Playing startup tone...");
        enableSpeakerAmp(true); // Enable amp for the tone
        
        // *** Temporarily lower the master volume for the tone ***
        float toneVolume = 0.05f; // Set master volume very low for the tone
        Serial.printf("Temporarily setting master volume to %.2f for tone\n", toneVolume);
        audioKit.setVolume(toneVolume); 
        
        delay(50); // Short delay for amp and volume change

        const int toneFrequency = 440; // A4 note
        const int toneDurationMs = 200; // Play for 200ms
        const int sampleRate = cfg.sample_rate; 
        const int numSamples = (sampleRate * toneDurationMs) / 1000;
        // *** Keep digital amplitude low as well ***
        const float amplitude = 0.1f; // Can keep this low (e.g., 10%)

        int16_t toneBuffer[128 * 2]; 
        int samplesGenerated = 0;

        while (samplesGenerated < numSamples) {
            // *** Initialize samplesInChunk for this iteration ***
            int samplesInChunk = 0;
            for (int i = 0; i < 128 && samplesGenerated < numSamples; ++i) {
                float time = (float)samplesGenerated / sampleRate;
                float sineValue = sin(2.0 * PI * toneFrequency * time);
                int16_t sampleValue = (int16_t)(sineValue * 32767.0f * amplitude);

                // Stereo output
                toneBuffer[i * 2] = sampleValue;     // Left channel
                toneBuffer[i * 2 + 1] = sampleValue; // Right channel

                samplesGenerated++;
                // *** Increment samplesInChunk ***
                samplesInChunk++;
            }
            // Write the chunk to the audio stream using the calculated samplesInChunk
            audioKit.write((uint8_t*)toneBuffer, samplesInChunk * 2 * sizeof(int16_t));
        }

        delay(50); // Short delay after tone
        enableSpeakerAmp(false); // Disable amp after tone
        
        // *** Restore the initial master volume ***
        Serial.printf("Restoring master volume to %.2f\n", initialVolumeFloat);
        audioKit.setVolume(initialVolumeFloat); 
        
        Serial.println("Startup tone finished.");
        // *** End of startup tone code ***
    }
    // --- END OF STEP 1 ---

    // --- STEP 2: Initialize SPIFFS ---
    Serial.println("Initializing SPIFFS...");
    if (!SPIFFS.begin(true)) {
        Serial.println("Failed to mount SPIFFS"); return; }
    readCredentials(); 
    // --- END OF STEP 2 ---

    // --- STEP 3: Initialize WiFi ---
    Serial.println("Initializing WiFi...");
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), password.c_str());
    Serial.print("Connecting to WiFi");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\n========================================");
    Serial.println("         WiFi Connected!");
    Serial.print("         IP: "); Serial.println(WiFi.localIP());
    Serial.println("========================================");
    // --- END OF STEP 3 ---

    // Synchronize time with NTP before SSL connections
    Serial.print("Synchronizing time with NTP...");
    timeClient.begin();
    int ntpTries = 0;
    while(!timeClient.update() && ntpTries < 20) { // Try up to 20 times
        delay(500);
        ntpTries++;
        Serial.print(".");
    }
    Serial.println();
    if(timeClient.getEpochTime() > 0) {
        Serial.print("NTP time: ");
        Serial.println(timeClient.getFormattedTime());
    } else {
        Serial.println("Failed to sync time with NTP!");
    }

    // --- STEP 4: Setup WebSocket ---
    Serial.println("Setting up WebSocket...");
    client.onMessage(onMessageCallback);
    client.onEvent(onEventsCallback);

    // Load the CA cert from SPIFFS
    File certFile = SPIFFS.open("/zello-io.crt", "r");
    if (!certFile) {
        Serial.println("Failed to open zello-io.crt - cannot establish secure connection!");
        // Don't proceed further without the cert
    } else {
        String caCert = certFile.readString();
        certFile.close();
        
        // Debug the certificate content
        Serial.println("CA certificate loaded:");
        Serial.println(caCert.substring(0, 64) + "..."); // Show first part
        
        // Set the certificate
        client.setCACert(caCert.c_str());
        Serial.println("CA certificate set for SSL connection");
        
        // Now try to connect
        Serial.println("Connecting to WebSocket server...");
        client.connect(websocket_server);
    }
    // --- END OF STEP 4 ---

    // --- STEP 5: Setup OTA and Buttons ---
    Serial.println("Setting up OTA and Buttons...");
    setupOTAWebServer();
    Serial.println("HTTP server started");
    Serial.print("Update interface available at http://"); Serial.println(WiFi.localIP());
    pinMode(PIN_PLAY, INPUT_PULLUP);
    pinMode(PIN_VOL_UP, INPUT_PULLUP);
    pinMode(PIN_VOL_DOWN, INPUT_PULLUP);
    pinMode(GPIO_PA_EN, OUTPUT); // Make sure pin is OUTPUT
    enableSpeakerAmp(false);     // Start with amplifier OFF
    // --- END OF STEP 5 ---

    Serial.println("\nSetup complete");
}

bool initOpusDecoder(int sampleRate) {
    int err;
    
    // Clean up any existing decoder
    if (decoder) {
        opus_decoder_destroy(decoder);
        decoder = nullptr;
    }
    
    // *** DEBUG: Log entry and sample rate ***
    Serial.printf("DEBUG: initOpusDecoder called with sampleRate=%d\n", sampleRate);

    Serial.printf("Creating OPUS decoder for %dHz\n", sampleRate);
    decoder = opus_decoder_create(sampleRate, CHANNELS, &err);
    
    // *** DEBUG: Log decoder creation result ***
    Serial.printf("DEBUG: opus_decoder_create returned: %d, decoder ptr: %p\n", err, decoder);

    if (err != OPUS_OK || !decoder) {
        Serial.printf("Failed to create decoder: %d (%s)\n", 
            err, opus_strerror(err));
        return false;
    }

    // Set decoder gain and bit depth
    err = opus_decoder_ctl(decoder, OPUS_SET_GAIN(0)); // *** Set gain to 0dB (no change) ***
    if (err != OPUS_OK) {
        Serial.printf("Failed to set gain: %s\n", opus_strerror(err));
    }
    
    err = opus_decoder_ctl(decoder, OPUS_SET_LSB_DEPTH(16));
    if (err != OPUS_OK) {
        Serial.printf("Failed to set bit depth: %s\n", opus_strerror(err));
    }

    // Allocate PCM buffer
    pcmBufferSize = MAX_FRAME_SIZE * CHANNELS;
    if (pcmBuffer) {
        free(pcmBuffer);
        pcmBuffer = nullptr; // *** DEBUG: Ensure pointer is nulled after free ***
    }
    // *** DEBUG: Log buffer allocation size ***
    Serial.printf("DEBUG: Allocating PCM buffer (size=%d samples, %d bytes)\n", pcmBufferSize, pcmBufferSize * sizeof(int16_t));
    pcmBuffer = (int16_t*)malloc(pcmBufferSize * sizeof(int16_t));

    // *** DEBUG: Log buffer allocation result ***
    Serial.printf("DEBUG: pcmBuffer malloc result: %p\n", pcmBuffer);

    if (!pcmBuffer) {
        Serial.println("Failed to allocate PCM buffer");
        opus_decoder_destroy(decoder);
        decoder = nullptr;
        return false;
    }

    Serial.println("OPUS decoder initialized successfully");
    return true;
}

void setVolume(uint8_t vol) {
    volume = constrain(vol, 0, 63); 
    float vol_float = volume / 63.0f; // Convert to 0.0 - 1.0 range
    Serial.printf("Setting volume to %d (%.2f)\n", volume, vol_float);
    if (!audioKit.setVolume(vol_float)) { // Check return value if available
         Serial.println("WARNING: Failed to set volume!");
    }
}

// volumeUp() and volumeDown() should work as they call the updated setVolume()
void volumeUp() {
    setVolume(volume + volume_step);
}

void volumeDown() {
    setVolume(volume - volume_step);
}


// Add this function implementation
void enableSpeakerAmp(bool enable) {
    // *** DEBUG: Log entry and requested state ***
    Serial.printf("DEBUG: enableSpeakerAmp called with enable=%s\n", enable ? "true" : "false");

    digitalWrite(GPIO_PA_EN, enable ? HIGH : LOW);

    // *** DEBUG: Log state AFTER writing to pin ***
    Serial.printf("Speaker amplifier %s (GPIO%d=%s)\n", 
                  enable ? "ENABLED" : "DISABLED", 
                  GPIO_PA_EN, 
                  digitalRead(GPIO_PA_EN) ? "HIGH" : "LOW");
    
    // Force a short delay to let amplifier stabilize
    delay(50);
    
    // Check if the amp enable pin is at the expected level
    if (digitalRead(GPIO_PA_EN) != (enable ? HIGH : LOW)) {
        Serial.println("WARNING: Amplifier control pin not at expected state!");
    }
}

// Add these near the top with other declarations
bool lastPlayState = HIGH;
bool lastVolUpState = HIGH;
bool lastVolDownState = HIGH;

// In your loop() function, replace the button handling section:
void loop() {
    // Handle WebSocket messages
    client.poll();

    // Handle OTA Web Server
    server.handleClient();

    // Handle button inputs
    static unsigned long lastButtonCheck = 0;
    unsigned long now = millis();
    if (now - lastButtonCheck >= 50) {  // Check buttons every 50ms
        lastButtonCheck = now;
        
        // Read button states with debouncing
        bool currentPlayState = digitalRead(PIN_PLAY);
        bool currentVolUpState = digitalRead(PIN_VOL_UP);
        bool currentVolDownState = digitalRead(PIN_VOL_DOWN);

        // Play button (Log press for now)
        if (currentPlayState == LOW && lastPlayState == HIGH) {
            Serial.println("Play/Mute button pressed");
            // TODO: Implement mute/unmute using audioKit if needed/available
            // e.g., audioKit.setVolume(0.0f); or audioKit.mute();
        }
        lastPlayState = currentPlayState;

        // Volume Up button
        if (currentVolUpState == LOW && lastVolUpState == HIGH) {
            volumeUp();
        }
        lastVolUpState = currentVolUpState;

        // Volume Down button
        if (currentVolDownState == LOW && lastVolDownState == HIGH) {
            volumeDown();
        }
        lastVolDownState = currentVolDownState;
    }

    // Rest of your existing loop code for WiFi/WebSocket checks
    // ...
}