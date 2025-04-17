#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoWebsockets.h>
#include <FS.h>
#include <SPIFFS.h>
#include <WebServer.h> // Include the WebServer library
#include <Update.h>  // Add this for OTA functionality
#include <mbedtls/base64.h> // Add this include for base64 decoding
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"

// Audio-tools includes for handling OPUS
#include "AudioTools.h"
// Include AudioBoard driver and the stream wrapper
#include "AudioBoard.h" // Assumes this is findable by the compiler
// Correct include path based on the example
#include "AudioTools/AudioLibs/AudioBoardStream.h" 
// For OPUS decoding
#include "AudioTools/AudioCodecs/CodecOpus.h"

// #include <WiFiUdp.h> // Commented out as NTP is removed
// #include <NTPClient.h> // Already commented out

// Add this function to help with UTF-8 debugging
void printUtf8HexBytes(const char* str, const char* label) {
  Serial.printf("%s: ", label);
  for (int i = 0; str[i] != 0; i++) {
    Serial.printf("%02X ", (uint8_t)str[i]);
  }
  Serial.println();
}

#define OPUS_BUFFER_SIZE 8192
#define MIN_BUFFER_SIZE 1024

#define IIS_SCLK 27
#define IIS_LCLK 26
#define IIS_DSIN 25

#define IIC_CLK 32 // Fixed value (was IIC_CLK 323)
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

#define FIRMWARE_VERSION "1.0.3"  // Increment version for this fix

// Volume control settings (only define once)
static uint8_t volume = 40;        // Start at ~63% volume (0-63 range)
const uint8_t volume_step = 5;     // Larger steps for quicker adjustment
float initialVolumeFloat = 0.63f;  // Store the initial float volume set in setup

using namespace websockets;

// Forward declarations and structures
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
    uint8_t frameCount;    // Should be 60ms
    uint8_t frameDuration; // Should be 20ms
};

// Global variables
WebServer server(80);
// Use AudioBoardStream wrapping the specific AudioBoard instance for AC101
// Note: AudioKitAC101 is defined in AudioBoard.h from audio-driver library
audio_tools::AudioBoardStream out(audio_driver::AudioKitAC101); 
// Add Audio-tools related objects - update to use correct namespaces
audio_tools::OpusAudioDecoder opusDecoder;
audio_tools::EncodedAudioStream *decoderStream = nullptr; // Will connect decoder to audio output
audio_tools::AudioInfo audioInfo;

// Add these global variables near the top with other declarations
unsigned long streamStartTime = 0;
unsigned long streamDuration = 0;
size_t totalBytesReceived = 0;
int totalPacketsReceived = 0;
bool isValidAudioStream = false;
int binaryPacketCount = 0;

// Websocket and Zello-related variables
WebsocketsClient client;
String ssid;
String password;
String token;
const char* websocket_server = "wss://zello.io/ws"; // Use wss:// for secure WebSocket

// Add these variables for Zello credentials
String zelloUsername = "Gabriel Huang";  // Default value
String zelloPassword = "22433897";      // Default value
String zelloChannel = "ZELLO無線聯合網";   // Default value

// Replace these variables for OPUS handling
bool decoderInitialized = false;

// NTP client variables - Commented out
// WiFiUDP ntpUDP;
// NTPClient timeClient(ntpUDP, "pool.ntp.org", 0, 60000); // UTC, update every 60s

// Add these global variables for audio enhancement
bool enhanceAudio = true;  // Enable audio enhancement by default
uint8_t enhancementProfile = 1; // 0=None, 1=Voice, 2=Music

// Add these global variables with the other globals
unsigned long lastPingTime = 0;
const unsigned long PING_INTERVAL = 30000; // Send ping every 30 seconds

// Add variables for button state tracking
bool lastPlayState = HIGH;
bool lastVolUpState = HIGH;
bool lastVolDownState = HIGH;

// Add these global variables near the top with other globals
String caCertificate;  // Store the CA certificate globally for reconnection attempts

// Forward declarations for functions
void readCredentials();
void setupOTAWebServer();
OpusPacket findNextOpusPacket(const uint8_t* data, size_t len);
bool initOpusDecoder(int sampleRate);
void enableSpeakerAmp(bool enable);
void volumeUp();
void volumeDown();
void setVolume(uint8_t vol);
void enhanceVoiceAudio(int16_t* buffer, int samples);
void onMessageCallback(WebsocketsMessage message); 
bool connectWebSocket();  // Add this missing declaration

// Update the validateOpusPacket function
bool validateOpusPacket(const uint8_t* data, size_t len) {
    if (len < 2) return false;
    // Debug output for first few packets
    uint8_t toc = data[0];
    uint8_t config = toc >> 3;        // First 5 bits
    uint8_t s = (toc >> 2) & 0x1;     // 1 bit
    uint8_t c = toc & 0x3;            // Last 2 bits

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

void onEventsCallback(WebsocketsEvent event, String data) {
    if (event == WebsocketsEvent::ConnectionOpened) {
        Serial.println("Connection Opened");
        // Logon message with UTF-8 channel name
        char logon[1024];
        sprintf(logon, "{\"command\": \"logon\",\"seq\": 1,\"auth_token\": \"%s\",\"username\": \"%s\",\"password\": \"%s\",\"channel\": \"%s\"}", 
               token.c_str(), 
               zelloUsername.c_str(),
               zelloPassword.c_str(),
               zelloChannel.c_str());
        // Remove the debug printing which shows auth token and other sensitive info
        // printUtf8HexBytes(logon, "Logon message UTF-8 bytes");
        client.send(logon);
    } else if (event == WebsocketsEvent::ConnectionClosed) {
        Serial.println("Connection Closed");
    } else if (event == WebsocketsEvent::GotPing) {
        Serial.println("Got Ping - Sending Pong");
        client.pong(); // This is correct - respond to ping with pong
    } else if (event == WebsocketsEvent::GotPong) {
        // Just log it, don't send anything back
        Serial.println("Got Pong - Connection is active"); 
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

    File wifiFile = SPIFFS.open("/wifi_credentials.ini", "r");
    if (!wifiFile) {
        Serial.println("Failed to open wifi_credentials.ini");
        return;
    }

    // UTF-8 BOM handling
    if (wifiFile.available() >= 3) {
        uint8_t bom[3];
        wifiFile.read(bom, 3);
        // Check for UTF-8 BOM (EF BB BF)
        if (bom[0] != 0xEF || bom[1] != 0xBB || bom[2] != 0xBF) {
            // Not a BOM, reset file position
            wifiFile.seek(0);
        }
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
            } else if (key == "channel") {
                zelloChannel = value;
                Serial.print("Channel from config: ");
                Serial.println(zelloChannel);
                printUtf8HexBytes(zelloChannel.c_str(), "Channel UTF-8 bytes");
            } else if (key == "username") {
                zelloUsername = value;
            } else if (key == "password_zello") {
                zelloPassword = value;
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

void enhanceVoiceAudio(int16_t* buffer, int samples) {
    // Skip if enhancement is disabled
    if (!enhanceAudio) return;

    static int16_t prevSample = 0;
    static int16_t prevSamples[3] = {0, 0, 0};

    switch (enhancementProfile) {
        case 0: // No enhancement
            return;
        case 1: { // Voice profile - boost high frequencies for clarity
            // Simple high-pass filter and treble boost for voice clarity
            const float highBoost = 1.5f;  // High frequency boost factor
            for (int i = 0; i < samples; i++) {
                // Simple first-order high-pass filter
                int16_t highpass = buffer[i] - prevSample;
                prevSample = buffer[i];
                // Mix original with high-pass filtered signal (boost high frequencies)
                buffer[i] = constrain(buffer[i] + (int16_t)(highpass * highBoost), -32768, 32767);
            }
            break;
        }
        case 2: { // Music profile - gentle enhancement
            // Simple presence boost (mid-high frequencies)
            const float presenceBoost = 1.2f;
            for (int i = 0; i < samples; i++) {
                // Simple band-pass approximation
                int16_t highMid = buffer[i] - ((prevSamples[0] + prevSamples[1] + prevSamples[2]) / 3);
                // Update delay line
                prevSamples[2] = prevSamples[1];
                prevSamples[1] = prevSamples[0];
                prevSamples[0] = buffer[i];
                // Mix original with high-mid boosted signal
                buffer[i] = constrain(buffer[i] + (int16_t)(highMid * presenceBoost), -32768, 32767);
            }
            break;
        }
    }
}

// Add this function before setup()
bool connectWebSocket() {
    if (caCertificate.length() == 0) {
        // Only load the certificate from SPIFFS if we haven't already
        File certFile = SPIFFS.open("/zello-io.crt", "r");
        if (!certFile) {
            Serial.println("Failed to open zello-io.crt - cannot establish secure connection!");
            return false;
        }
        
        caCertificate = certFile.readString();
        certFile.close();
        Serial.println("CA certificate loaded:");
        Serial.println(caCertificate.substring(0, 64) + "..."); // Show first part
    }

    Serial.println("Connecting to WebSocket server...");
    // We need to set the certificate every time we connect
    client.setCACert(caCertificate.c_str());
    Serial.println("CA certificate set for SSL connection");

    bool connected = client.connect(websocket_server);
    if (!connected) {
        Serial.println("WebSocket connection failed!");
        // Check if certificate has issues
        if (caCertificate.indexOf("BEGIN CERTIFICATE") == -1 || 
            caCertificate.indexOf("END CERTIFICATE") == -1) {
            Serial.println("ERROR: Certificate appears to be invalid. Check the format!");
        }
    }
    return connected;
}

void setup() {
    Serial.begin(115200);
    delay(100);
    Serial.println("\n\n=== Booting Zello Client (using Audio-tools with AudioBoardStream) ===");
    
    // --- STEP 1: Initialize Audio ---
    Serial.println("Initializing Audio...");
    // Use the AudioBoardStream 'out' for configuration and initialization
    auto cfg = out.defaultConfig(TX_MODE); // Use TX_MODE for output
    cfg.sample_rate = 48000;
    cfg.channels = 2;
    cfg.bits_per_sample = 16;
    // Print the configuration 
    Serial.println("Attempting Audio Config:");
    Serial.printf("- Sample Rate: %d\n", cfg.sample_rate);
    Serial.printf("- Channels: %d\n", cfg.channels);
    Serial.printf("- Bits/Sample: %d\n", cfg.bits_per_sample);
    // Pins are handled by the underlying AudioBoard instance

    // Begin the AudioBoardStream instance
    if (!out.begin(cfg)) { 
        Serial.println("AudioBoardStream initialization FAILED! Halting.");
        while(1) { delay(1000); }
    } else {
        Serial.println("AudioBoardStream initialized successfully.");
        initialVolumeFloat = volume / 63.0f;
        // Set volume using the AudioBoardStream instance
        out.setVolume(initialVolumeFloat); 
        Serial.printf("Initial volume set to %d (%.2f)\n", volume, initialVolumeFloat);
        
        // Play a startup tone
        Serial.println("Playing startup tone...");
        enableSpeakerAmp(true);  // Enable amp for the tone
        float toneVolume = 0.05f;  // Set master volume very low for the tone
        Serial.printf("Temporarily setting master volume to %.2f for tone\n", toneVolume);
        out.setVolume(toneVolume); 
        delay(50);  // Short delay for amp and volume change
        
        // Generate tone
        const int toneFrequency = 440;  // A4 note
        const int toneDurationMs = 200;  // Play for 200ms
        const int sampleRate = cfg.sample_rate;
        const int numSamples = (sampleRate * toneDurationMs) / 1000;
        const float amplitude = 0.1f;  // 10% amplitude
        int16_t toneBuffer[128 * 2];
        int samplesGenerated = 0;
        
        while (samplesGenerated < numSamples) {
            int samplesInChunk = 0;
            for (int i = 0; i < 128 && samplesGenerated < numSamples; ++i) {
                float time = (float)samplesGenerated / sampleRate;
                float sineValue = sin(2.0 * PI * toneFrequency * time);
                int16_t sampleValue = (int16_t)(sineValue * 32767.0f * amplitude);
                // Stereo output
                toneBuffer[i * 2] = sampleValue;      // Left channel
                toneBuffer[i * 2 + 1] = sampleValue;  // Right channel
                samplesGenerated++;
                samplesInChunk++;
            }
            // Write using the AudioBoardStream 'out'
            out.write((uint8_t*)toneBuffer, samplesInChunk * 2 * sizeof(int16_t)); 
        }
        
        delay(50);  // Short delay after tone
        enableSpeakerAmp(false);  // Disable amp after tone
        Serial.printf("Restoring master volume to %.2f\n", initialVolumeFloat);
        out.setVolume(initialVolumeFloat); 
        Serial.println("Startup tone finished.");
    }
    
    // --- END OF STEP 1 ---
    // --- STEP 2: Initialize SPIFFS ---
    Serial.println("Initializing SPIFFS...");
    if (!SPIFFS.begin(true)) {
        Serial.println("Failed to mount SPIFFS");
        return;
    }
    readCredentials();
    // --- END OF STEP 2 ---
    // --- STEP 3: Initialize WiFi ------
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
    // Synchronize time with NTP before SSL connection - Commented out
    /*
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
        Serial.print("NTP time: "); Serial.println(timeClient.getFormattedTime());
    } else {
        Serial.println("Failed to sync time with NTP!");
    }
    */
    // --- STEP 4: Setup WebSocket ---
    Serial.println("Setting up WebSocket...");
    client.onMessage(onMessageCallback);
    client.onEvent(onEventsCallback);
    // Connect to WebSocket server using our new function
    connectWebSocket();
    // --- END OF STEP 4 ---
    // --- STEP 5: Setup OTA and Buttons ---
    Serial.println("Setting up OTA and Buttons...");
    setupOTAWebServer();
    Serial.println("HTTP server started");
    Serial.print("Dashboard available at http://"); Serial.println(WiFi.localIP());
    Serial.print("OTA Update available at http://"); Serial.print(WiFi.localIP()); Serial.println("/ota");
    pinMode(PIN_PLAY, INPUT_PULLUP);
    pinMode(PIN_VOL_UP, INPUT_PULLUP);
    pinMode(PIN_VOL_DOWN, INPUT_PULLUP);
    pinMode(GPIO_PA_EN, OUTPUT); // Make sure pin is OUTPUT
    enableSpeakerAmp(false);     // Start with amplifier OFF
    // --- END OF STEP 5 ---
    Serial.println("\nSetup complete");
}

void loop() {
    // Handle WebSocket messages and server
    if (client.available()) {
        client.poll();
        // Send ping periodically to keep connection alive
        unsigned long currentTime = millis();
        if (currentTime - lastPingTime > PING_INTERVAL) {
            Serial.println("Sending ping to keep connection alive");
            client.ping();
            lastPingTime = currentTime;
        }
    } else {
        // Try to reconnect if not connected
        static unsigned long lastReconnectAttempt = 0;
        static int reconnectAttempts = 0;
        unsigned long currentTime = millis();
        
        if (currentTime - lastReconnectAttempt > 5000) { // Try every 5 seconds
            Serial.println("WebSocket disconnected. Attempting to reconnect...");
            
            // Reset the client before attempting to reconnect
            client.close();
            delay(100); // Short delay to ensure cleanup
            
            if (connectWebSocket()) {
                reconnectAttempts = 0;
                Serial.println("WebSocket reconnected successfully!");
            } else {
                reconnectAttempts++;
                // If we've tried too many times, increase the delay
                if (reconnectAttempts > 5) {
                    Serial.println("Multiple reconnect failures. Increasing delay...");
                    delay(5000); // Additional delay after repeated failures
                    
                    // After several attempts, try to reload the certificate
                    if (reconnectAttempts % 3 == 0) {
                        Serial.println("Reloading certificate from storage...");
                        caCertificate = ""; // Force reload on next attempt
                    }
                }
            }
            lastReconnectAttempt = currentTime;
        }
    }

    // Handle button inputs
    unsigned long now = millis();
    static unsigned long lastButtonCheck = 0;
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
    // Handle OTA Web Server
    server.handleClient();
}

bool initOpusDecoder(int sampleRate) {
    // Log entry and sample rate
    Serial.printf("Initializing OPUS decoder with sampleRate=%d\n", sampleRate);
    
    // Clean up existing decoder if any
    if (decoderStream != nullptr) {
        delete decoderStream;
        decoderStream = nullptr;
    }
    // Set up audio info for the decoder
    audioInfo.sample_rate = sampleRate;
    audioInfo.channels = 1; // Start with mono from OPUS
    audioInfo.bits_per_sample = 16;
    
    // Create a new EncodedAudioStream using the AudioBoardStream 'out'
    decoderStream = new audio_tools::EncodedAudioStream(&out, &opusDecoder); 
    if (!decoderStream) {
        Serial.println("Failed to create decoder stream!");
        return false;
    }
    
    // Configure the decoder - output is potentially stereo (based on AudioKit)
    audio_tools::AudioInfo outputInfo;
    outputInfo.sample_rate = sampleRate; 
    outputInfo.channels = 2; // Output to stereo AudioKit
    outputInfo.bits_per_sample = 16;
    
    if (!decoderStream->begin(outputInfo)) {
        Serial.println("Failed to initialize decoder stream!");
        delete decoderStream;
        decoderStream = nullptr;
        return false;
    }
    
    Serial.println("OPUS decoder initialized successfully");
    decoderInitialized = true;
    return true;
}

void setVolume(uint8_t vol) {
    volume = constrain(vol, 0, 63);
    float vol_float = volume / 63.0f; // Convert to 0.0 - 1.0 range
    Serial.printf("Setting volume to %d (%.2f)\n", volume, vol_float);
    
    // Use AudioBoardStream instance to set volume instead
    if (!out.setVolume(vol_float)) { // Check return value if available
         Serial.println("WARNING: Failed to set volume!");
    }
}

void volumeUp() {
    setVolume(volume + volume_step);
}

void volumeDown() {
    setVolume(volume - volume_step);
}

void enableSpeakerAmp(bool enable) {
    // *** DEBUG: Log entry and requested state ***
    Serial.printf("DEBUG: enableSpeakerAmp called with enable=%s\n", enable ? "true" : "false");
    digitalWrite(GPIO_PA_EN, enable ? HIGH : LOW);
    // *** DEBUG: Log state AFTER writing to pin ***
    Serial.printf("Speaker amplifier %s (GPIO%d=%s)\n", 
                  enable ? "ENABLED" : "DISABLED", 
                  GPIO_PA_EN, digitalRead(GPIO_PA_EN) ? "HIGH" : "LOW");
    // Force a short delay to let amplifier stabilize
    delay(50);
    // Check if the amp enable pin is at the expected level
    if (digitalRead(GPIO_PA_EN) != (enable ? HIGH : LOW)) {
        Serial.println("WARNING: Amplifier control pin not at expected state!");
    }
}

void onMessageCallback(WebsocketsMessage message) {
    if (message.isBinary()) {
        // Handle binary message (audio data)
        size_t msgLen = message.length();
        const uint8_t* rawData = (const uint8_t*)message.c_str();
        
        // Print first packet details
        if (binaryPacketCount == 0) {
            Serial.println("\nFirst packet details:");
            Serial.printf("Total length: %d bytes\n", msgLen);
            Serial.printf("Packet type: 0x%02X\n", rawData[0]);
            Serial.printf("OPUS data length: %d bytes\n", msgLen - 9);
        }
        
        // Validate packet type
        if (rawData[0] != 0x01) {
            Serial.printf("Invalid packet type: 0x%02X\n", rawData[0]);
            return;
        }
        
        // Extract OPUS data (skip 9-byte header)
        const uint8_t* opusData = rawData + 9;
        size_t opusLen = msgLen - 9;
        
        // Print packet details for first few packets
        if (binaryPacketCount < DETAILED_PACKET_COUNT) {
            Serial.printf("\nOPUS Packet %d:\n", binaryPacketCount);
            Serial.printf("- Length: %d bytes\n", opusLen);
            Serial.printf("- First 8 bytes: ");
            for (int i = 0; i < min(8, (int)opusLen); i++) {
                Serial.printf("%02X ", opusData[i]);
            }
            Serial.println();
        }
        
        // Check for valid packet size
        if (opusLen < 2) {
            Serial.println("OPUS packet too small");
            return;
        }
        if (opusLen > MAX_PACKET_SIZE) {
            Serial.printf("OPUS packet too large: %d > %d\n", opusLen, MAX_PACKET_SIZE);
            return;
        }
        
        // Using Audio-tools EncodedAudioStream to decode OPUS
        if (decoderInitialized && decoderStream) {
            size_t bytes_written = decoderStream->write(opusData, opusLen);
            // Check for decode errors
            if (bytes_written != opusLen) {
                Serial.printf("OPUS decode error: wrote %d of %d bytes\n", bytes_written, opusLen);
            } else if (binaryPacketCount == 0 || binaryPacketCount % 100 == 0) {
                Serial.printf("AudioTools decoder write: packet=%d, bytes=%d/%d\n",
                            binaryPacketCount, bytes_written, opusLen);
            }
        }
        
        // Update packet counters
        totalBytesReceived += opusLen;
        totalPacketsReceived++;
        binaryPacketCount++;
    } else {
        // Handle text message (JSON control messages)
        String msg = message.data();
            
        // Stream start message
        if (msg.indexOf("\"command\":\"on_stream_start\"") >= 0) {
            Serial.println("\n=== Stream Start Message ===");
            Serial.println(msg);
            Serial.println("===========================\n");
            
            // Extract codec header from JSON
            Serial.println("Attempting to find codec_header...");
            int headerStart = msg.indexOf("\"codec_header\":\"");
            if (headerStart >= 0) {
                headerStart += 16; // Skip past "codec_header":""
                int headerEnd = msg.indexOf("\"", headerStart);
                if (headerEnd > headerStart) {
                    String codecHeader = msg.substring(headerStart, headerEnd);
                    Serial.printf("Extracted Codec Header: [%s]\n", codecHeader.c_str());
                    
                    // Decode base64 header
                    size_t decodedLen = 0;
                    uint8_t decoded[4];
                    int decode_ret = mbedtls_base64_decode(decoded, 4, &decodedLen, 
                        (const uint8_t*)codecHeader.c_str(), codecHeader.length());
                    Serial.printf("Base64 decode result: %d, decoded length: %d\n", decode_ret, decodedLen);
                    if (decode_ret == 0 && decodedLen == 4) {
                        // Parse OpusConfig
                        OpusConfig config;
                        config.sampleRate = decoded[0] | (decoded[1] << 8);
                        config.framesPerPacket = decoded[2];
                        config.frameSizeMs = decoded[3];
                        
                        Serial.printf("Opus Config: %dHz, %d frames/packet, %dms/frame\n",
                            config.sampleRate, config.framesPerPacket, config.frameSizeMs);
                        
                        // Configure audio output using the AudioBoardStream instance
                        auto cfg = out.defaultConfig(TX_MODE);
                        cfg.sample_rate = config.sampleRate;
                        cfg.bits_per_sample = 16;
                        cfg.channels = 2;
                        
                        // Re-initialize the AudioBoardStream with the new config
                        if (!out.begin(cfg)) { 
                            Serial.println("WARNING: Failed to apply updated audio config!");
                        } else {
                            Serial.printf("Audio parameters updated (%dHz, 16bit, Stereo).\n", config.sampleRate);
                            delay(10);
                            
                            // Set initial stream volume using the AudioBoardStream instance
                            float streamVolume = 0.2f;
                            Serial.printf("Setting stream volume to %.2f\n", streamVolume);
                            out.setVolume(streamVolume); 
                        }
                        
                        // Initialize Opus decoder
                        if (!initOpusDecoder(config.sampleRate)) {
                            Serial.println("Failed to initialize Opus decoder");    
                            return;
                        }
                        // Enable amplifier
                        enableSpeakerAmp(true);
                    } else {
                        Serial.println("Base64 decode FAILED or length != 4.");
                    }
                } else {
                    Serial.println("Could not find end quote for codec_header.");
                }
            } else {
                Serial.println("Could not find start of codec_header string.");
            }
            // Reset stream counters
            streamStartTime = millis();
            totalBytesReceived = 0;
            totalPacketsReceived = 0;
            binaryPacketCount = 0;
            isValidAudioStream = true;
        }
        // Stream stop message
        else if (msg.indexOf("\"command\":\"on_stream_stop\"") >= 0) {
            Serial.println("\n=== Stream Stop Message ===");
            Serial.println(msg);
            Serial.println("===========================\n");
            
            // Calculate stream stats
            streamDuration = millis() - streamStartTime;
            
            Serial.println("\n=== Stream Statistics ===");
            Serial.printf("Duration: %.2f seconds\n", streamDuration/1000.0);
            Serial.printf("Total packets: %d\n", totalPacketsReceived);
            Serial.printf("OPUS bytes: %d\n", totalBytesReceived);
            Serial.printf("Packet rate: %.1f packets/s\n", 
                        (totalPacketsReceived * 1000.0) / streamDuration);
            Serial.println("=====================\n");
            
            // Clean up resources
            isValidAudioStream = false;
            if (decoderStream) {
                decoderStream->end();
                delete decoderStream;
                decoderStream = nullptr;
                decoderInitialized = false;
            }
            
            // Disable amplifier
            Serial.println("Disabling speaker amplifier for stream stop...");
            enableSpeakerAmp(false);
            // Restore volume using the AudioBoardStream instance
            Serial.printf("Restoring initial volume to %.2f\n", initialVolumeFloat);
            out.setVolume(initialVolumeFloat); 
        }
        // Channel status message
        else if (msg.indexOf("\"command\":\"channel_status\"") >= 0) {
            Serial.println("\n=== Channel Status ===");
            Serial.println(msg);
            Serial.println("===================\n");
            // Extract channel name
            int channelStart = msg.indexOf("\"channel\":\"");
            if (channelStart >= 0) {
                channelStart += 11;
                int channelEnd = msg.indexOf("\"", channelStart);
                if (channelEnd > channelStart) {
                    String channel = msg.substring(channelStart, channelEnd);
                    Serial.printf("Connected to channel: %s (UTF-8)\n", channel.c_str());
                    
                    // Display channel name as hex bytes
                    Serial.print("Channel name in hex: ");
                    for (int i = 0; i < channel.length(); i++) {
                        Serial.printf("%02X ", (uint8_t)channel[i]);
                    }
                    Serial.println();
                }
            }
            Serial.println("===================\n");
        }
    }
}

// Implementation of the missing setupOTAWebServer function
void setupOTAWebServer() {
    // Main dashboard page
    server.on("/", HTTP_GET, []() {
        String html = "<!DOCTYPE html><html><head>";
        html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
        html += "<title>ESP32 Zello Client Dashboard</title>";
        html += "<style>";
        html += "body{font-family:Arial,sans-serif;margin:0;padding:20px;color:#e0e0e0;max-width:800px;margin:0 auto;background-color:#1e1e1e;}";
        html += "h1{color:#0099ff;margin-top:20px;margin-bottom:10px;}";
        html += "h2{color:#00ccff;margin-top:20px;margin-bottom:10px;}";
        html += ".stat-box{background:#2d2d2d;padding:15px;border-radius:5px;margin-bottom:15px;}";
        html += ".stat-grid{display:grid;grid-template-columns:1fr 1fr;gap:10px;}";
        html += ".stat-item{display:flex;justify-content:space-between;}";
        html += ".label{font-weight:bold;color:#c0c0c0;}";
        html += ".controls{margin-top:20px;padding:15px;background:#2a3a4a;border-radius:5px;}";
        html += ".btn{background:#0088cc;color:white;border:none;padding:8px 15px;margin-right:10px;margin-bottom:10px;border-radius:4px;cursor:pointer;}";
        html += ".btn:hover{background:#006699;}";
        html += "@media (max-width:600px){.stat-grid{grid-template-columns:1fr;}}";
        html += "</style></head><body>";
        html += "<h1>ESP32 Zello Client Dashboard</h1>";
        
        // System Information
        html += "<h2>System Information</h2>";
        html += "<div class='stat-box'><div class='stat-grid'>";
        html += "<div class='stat-item'><span class='label'>Uptime:</span><span>" + String(millis()/1000) + " seconds</span></div>";
        html += "<div class='stat-item'><span class='label'>Free Heap:</span><span>" + String(ESP.getFreeHeap()) + " bytes</span></div>";
        html += "<div class='stat-item'><span class='label'>Total Heap:</span><span>" + String(ESP.getHeapSize()) + " bytes</span></div>";
        html += "<div class='stat-item'><span class='label'>Flash Size:</span><span>" + String(ESP.getFlashChipSize()) + " bytes</span></div>";
        html += "<div class='stat-item'><span class='label'>Free Sketch Space:</span><span>" + String(ESP.getFreeSketchSpace()) + " bytes</span></div>";
        html += "<div class='stat-item'><span class='label'>ESP32 SDK:</span><span>" + String(ESP.getSdkVersion()) + "</span></div>";
        html += "<div class='stat-item'><span class='label'>Firmware Version:</span><span>" + String(FIRMWARE_VERSION) + "</span></div>";
        html += "</div></div>";
        
        // WiFi Information
        html += "<h2>WiFi Information</h2>";
        html += "<div class='stat-box'><div class='stat-grid'>";
        html += "<div class='stat-item'><span class='label'>WiFi SSID:</span><span>" + WiFi.SSID() + "</span></div>";
        html += "<div class='stat-item'><span class='label'>IP Address:</span><span>" + WiFi.localIP().toString() + "</span></div>";
        html += "<div class='stat-item'><span class='label'>MAC Address:</span><span>" + WiFi.macAddress() + "</span></div>";
        html += "<div class='stat-item'><span class='label'>WiFi RSSI:</span><span>" + String(WiFi.RSSI()) + " dBm</span></div>";
        html += "<div class='stat-item'><span class='label'>Gateway IP:</span><span>" + WiFi.gatewayIP().toString() + "</span></div>";
        html += "</div></div>";
        
        // Audio & Zello Status
        html += "<h2>Audio & Zello Status</h2>";
        html += "<div class='stat-box'><div class='stat-grid'>";
        html += "<div class='stat-item'><span class='label'>Current Volume:</span><span>" + String(volume) + "/63 (" + String(int(volume * 100 / 63)) + "%)</span></div>";
        html += "<div class='stat-item'><span class='label'>Speaker Amplifier:</span><span>" + String(digitalRead(GPIO_PA_EN) ? "ON" : "OFF") + "</span></div>";
        html += "<div class='stat-item'><span class='label'>Websocket Connected:</span><span>" + String(client.available() ? "Yes" : "No") + "</span></div>";
        html += "<div class='stat-item'><span class='label'>Active Audio Stream:</span><span>" + String(isValidAudioStream ? "Yes" : "No") + "</span></div>";
        
        // Audio Enhancement Status
        html += "<div class='stat-item'><span class='label'>Audio Enhancement:</span><span>" + String(enhanceAudio ? "ON" : "OFF") + "</span></div>";
        html += "<div class='stat-item'><span class='label'>Enhancement Profile:</span><span>";
        switch (enhancementProfile) {
            case 0: html += "None"; break;
            case 1: html += "Voice"; break;
            case 2: html += "Music"; break;
        }
        html += "</span></div>";
        
        html += "<div class='stat-item'><span class='label'>Total Packets Received:</span><span>" + String(totalPacketsReceived) + "</span></div>";
        html += "<div class='stat-item'><span class='label'>Current/Last Stream:</span><span>";
        if (isValidAudioStream) {
            html += String((millis() - streamStartTime)/1000.0, 1) + " sec (active)";
        } else {
            html += String(streamDuration/1000.0, 1) + " sec (ended)";
        }
        html += "</span></div>";
        html += "</div></div>";
        
        // Controls
        html += "<h2>Device Controls</h2>";
        html += "<div class='controls'>";
        html += "<button class='btn' onclick=\"window.location.href='/volume/up'\">Volume +</button>";
        html += "<button class='btn' onclick=\"window.location.href='/volume/down'\">Volume -</button>";
        html += "<button class='btn' onclick=\"window.location.href='/speaker/on'\">Speaker On</button>";
        html += "<button class='btn' onclick=\"window.location.href='/speaker/off'\">Speaker Off</button>";
        
        // Audio enhancement
        html += "<button class='btn' onclick=\"window.location.href='/audio/enhance/" + String(!enhanceAudio) + "'\">" + 
                String(enhanceAudio ? "Enhancement OFF" : "Enhancement ON") + "</button>";
        html += "<button class='btn' onclick=\"window.location.href='/audio/profile/next'\">Next Profile (" + 
                String(enhancementProfile == 0 ? "None" : enhancementProfile == 1 ? "Voice" : "Music") + ")</button>";
        
        html += "<button class='btn' onclick=\"window.location.href='/reconnect'\">Reconnect WS</button>";
        html += "<button class='btn' onclick=\"window.location.href='/ota'\">OTA Update</button>";
        html += "<button class='btn' onclick=\"if(confirm('Restart the device?')) window.location.href='/reboot'\">Reboot</button>";
        html += "<button class='btn' onclick=\"window.location.href='/config/wifi'\">WiFi Settings</button>";
        html += "<button class='btn' onclick=\"window.location.href='/config/zello'\">Zello Settings</button>";
        html += "</div>";
         
        // Auto-refresh
        html += "<p style='text-align:center;margin-top:20px;'><small>Auto-refreshing every 5 seconds</small></p>";
        html += "<script>setTimeout(function(){window.location.reload();}, 5000);</script>";
        html += "</body></html>";
        server.send(200, "text/html", html);
    });

    // Volume control endpoints
    server.on("/volume/up", HTTP_GET, []() {
        volumeUp();
        server.sendHeader("Location", "/");
        server.send(303);
    });
    
    server.on("/volume/down", HTTP_GET, []() {
        volumeDown();
        server.sendHeader("Location", "/");
        server.send(303);
    });
    
    // Speaker control endpoints
    server.on("/speaker/on", HTTP_GET, []() {
        enableSpeakerAmp(true);
        server.sendHeader("Location", "/");
        server.send(303);
    });
    
    server.on("/speaker/off", HTTP_GET, []() {
        enableSpeakerAmp(false);
        server.sendHeader("Location", "/");
        server.send(303);
    });
    
    // Audio enhancement endpoints
    server.on("/audio/enhance/0", HTTP_GET, []() {
        enhanceAudio = false;
        server.sendHeader("Location", "/");
        server.send(303);
    });
    
    server.on("/audio/enhance/1", HTTP_GET, []() {
        enhanceAudio = true;
        server.sendHeader("Location", "/");
        server.send(303);
    });
    
    server.on("/audio/profile/next", HTTP_GET, []() {
        enhancementProfile = (enhancementProfile + 1) % 3;  // Cycle through profiles
        server.sendHeader("Location", "/");
        server.send(303);
    });
    
    // WebSocket reconnection endpoint
    server.on("/reconnect", HTTP_GET, []() {
        if (!client.available()) {
            client.connect(websocket_server);
        }
        server.sendHeader("Location", "/");
        server.send(303);
    });
    
    // Reboot endpoint
    server.on("/reboot", HTTP_GET, []() {
        server.send(200, "text/html", "<html><body><h1>Device is restarting...</h1><script>setTimeout(function(){window.location.href='/';}, 8000);</script></body></html>");
        delay(500);
        ESP.restart();
    });
    
    // OTA update endpoint
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
                return;
            }
        } else if (upload.status == UPLOAD_FILE_WRITE) {
            if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
                Update.printError(Serial);
                return;
            }
        } else if (upload.status == UPLOAD_FILE_END) {
            if (Update.end(true)) {
                Serial.printf("Update Success: %u\nRebooting...\n", upload.totalSize);
            } else {
                Update.printError(Serial);
            }
        }
        
        yield();
    });
    
    // OTA update page
    server.on("/ota", HTTP_GET, []() {
        String html = "<!DOCTYPE html><html><head>";
        html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
        html += "<title>ESP32 OTA Update</title>";
        html += "<style>";
        html += "body{font-family:Arial,sans-serif;margin:20px;background:#222;color:#fff;}";
        html += "h1{color:#0099ff;}";
        html += "form{margin:20px 0;padding:15px;background:#333;border-radius:5px;}";
        html += "input[type=file]{margin:10px 0;}";
        html += "input[type=submit]{background:#0088cc;color:#fff;padding:10px 15px;border:none;border-radius:4px;cursor:pointer;}";
        html += "progress{width:100%;height:20px;}";
        html += "</style></head><body>";
        html += "<h1>ESP32 Firmware Update</h1>";
        html += "<p>Current Version: " + String(FIRMWARE_VERSION) + "</p>";
        html += "<form method='POST' action='/update' enctype='multipart/form-data' id='upload_form'>";
        html += "<input type='file' name='update'><br>";
        html += "<progress id='prog' value='0' max='100'></progress><br>";
        html += "<input type='submit' value='Update'>";
        html += "</form>";
        html += "<div id='status'></div>";
        html += "<script>";
        html += "var form = document.getElementById('upload_form');";
        html += "var prog = document.getElementById('prog');";
        html += "var stat = document.getElementById('status');";
        html += "form.addEventListener('submit', function(e) {";
        html += "  e.preventDefault();";
        html += "  var xhr = new XMLHttpRequest();";
        html += "  xhr.open('POST', '/update');";
        html += "  xhr.upload.addEventListener('progress', function(e) {";
        html += "    prog.value = e.loaded / e.total * 100;";
        html += "    stat.innerHTML = 'Upload: ' + Math.round(prog.value) + '%';";
        html += "  });";
        html += "  xhr.onreadystatechange = function() {";
        html += "    if (xhr.readyState === 4) {";
        html += "      if (xhr.status === 200) {";
        html += "        stat.innerHTML = 'Update successful! Rebooting...';";
        html += "        setTimeout(function(){window.location.href='/';}, 10000);";
        html += "      } else {";
        html += "        stat.innerHTML = 'Update failed';";
        html += "      }";
        html += "    }";
        html += "  };";
        html += "  xhr.send(new FormData(form));";
        html += "});";
        html += "</script>";
        html += "<p><a href='/'>&larr; Back to Dashboard</a></p>";
        html += "</body></html>";
        server.send(200, "text/html", html);
    });

    // Add WiFi configuration page
    server.on("/config/wifi", HTTP_GET, []() {
        String html = "<!DOCTYPE html><html><head>";
        html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
        html += "<title>WiFi Configuration</title>";
        html += "<style>";
        html += "body{font-family:Arial,sans-serif;margin:0;padding:20px;color:#e0e0e0;max-width:800px;margin:0 auto;background-color:#1e1e1e;}";
        html += "h1{color:#0099ff;margin-top:20px;margin-bottom:10px;}";
        html += "h2{color:#00ccff;margin-top:20px;margin-bottom:10px;}";
        html += ".form-box{background:#2d2d2d;padding:15px;border-radius:5px;margin-bottom:15px;}";
        html += "label{display:block;margin-bottom:5px;color:#c0c0c0;}";
        html += "input[type=text], input[type=password]{width:100%;padding:8px;margin-bottom:15px;background:#3d3d3d;border:1px solid #555;color:#e0e0e0;border-radius:3px;}";
        html += ".btn{background:#0088cc;color:white;border:none;padding:8px 15px;margin-right:10px;border-radius:4px;cursor:pointer;}";
        html += ".btn:hover{background:#006699;}";
        html += ".note{background:#3a3a3a;padding:10px;margin-top:15px;border-left:3px solid #0099ff;font-size:0.9em;}";
        html += "</style></head><body>";
        html += "<h1>WiFi Configuration</h1>";

        html += "<div class='form-box'>";
        html += "<form method='POST' action='/config/wifi/save'>";
        
        // WiFi SSID
        html += "<label for='ssid'>WiFi SSID:</label>";
        html += "<input type='text' id='ssid' name='ssid' value='" + ssid + "'>";
        
        // WiFi password
        html += "<label for='password'>WiFi Password:</label>";
        html += "<input type='password' id='password' name='password' value='" + password + "'>";
        
        html += "<button type='submit' class='btn'>Save Configuration</button>";
        html += "<button type='button' class='btn' onclick=\"window.location.href='/'\">Cancel</button>";
        html += "</form>";
        
        html += "<div class='note'>";
        html += "<p><strong>Note:</strong> After saving, the device will reboot to apply the new WiFi settings.</p>";
        html += "</div>";
        
        html += "</div>";
        html += "<p><a href='/'>&larr; Back to Dashboard</a></p>";
        html += "</body></html>";
        server.send(200, "text/html", html);
    });

    // Handle saving WiFi configuration
    server.on("/config/wifi/save", HTTP_POST, []() { 
        bool needReboot = false;
        
        // Get form values
        if (server.hasArg("ssid") && server.arg("ssid") != ssid) {
            ssid = server.arg("ssid");
            needReboot = true;
        }
        
        if (server.hasArg("password") && server.arg("password") != password) {
            password = server.arg("password");
            needReboot = true;
        }
        
        // Save to file if changes were made
        if (needReboot) {
            // Read existing file content to preserve other settings
            File wifiFile = SPIFFS.open("/wifi_credentials.ini", "r");
            String fileContent = "";
            
            if (wifiFile) {
                while (wifiFile.available()) {
                    fileContent += wifiFile.readStringUntil('\n') + "\n";
                }
                wifiFile.close();
                
                // Update or add WiFi credentials
                bool ssidFound = false;
                bool passwordFound = false;
                
                // Create a temporary buffer for the new content
                String newContent = "";
                int startPos = 0;
                int endPos = 0;
                
                while ((endPos = fileContent.indexOf('\n', startPos)) != -1) {
                    String line = fileContent.substring(startPos, endPos + 1);
                    if (line.indexOf("ssid=") == 0) {
                        newContent += "ssid=" + ssid + "\n";
                        ssidFound = true;
                    } 
                    else if (line.indexOf("password=") == 0) {
                        newContent += "password=" + password + "\n";
                        passwordFound = true;
                    } 
                    else {
                        newContent += line;
                    }
                    startPos = endPos + 1;
                }
                
                // Add any missing entries
                if (!ssidFound) {
                    newContent += "ssid=" + ssid + "\n";
                }
                if (!passwordFound) {
                    newContent += "password=" + password + "\n";
                }
                
                // Write the updated content back
                wifiFile = SPIFFS.open("/wifi_credentials.ini", "w");
                if (wifiFile) {
                    wifiFile.print(newContent);
                    wifiFile.close();
                    Serial.println("WiFi credentials updated in wifi_credentials.ini");
                }
            }
            
            // Send response page and reboot
            server.send(200, "text/html", "<html><body><h2>WiFi Settings Updated</h2><p>The device is restarting to apply the new settings...</p><script>setTimeout(function(){window.location.href='/';}, 10000);</script></body></html>");
            delay(1000);
            ESP.restart();
        } else {
            // Redirect back to the dashboard if no changes
            server.sendHeader("Location", "/");
            server.send(303);
        }
    });

    // Add Zello configuration page - Fixed UTF-8 handling
    server.on("/config/zello", HTTP_GET, []() {
        String html = "<!DOCTYPE html><html><head>";
        html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
        html += "<meta charset='UTF-8'>"; // Add UTF-8 charset declaration
        html += "<title>Zello Configuration</title>";
        html += "<style>";
        html += "body{font-family:Arial,sans-serif;margin:0;padding:20px;color:#e0e0e0;max-width:800px;margin:0 auto;background-color:#1e1e1e;}";
        html += "h1{color:#0099ff;margin-top:20px;margin-bottom:10px;}";
        html += "h2{color:#00ccff;margin-top:20px;margin-bottom:10px;}";
        html += ".form-box{background:#2d2d2d;padding:15px;border-radius:5px;margin-bottom:15px;}";
        html += "label{display:block;margin-bottom:5px;color:#c0c0c0;}";
        html += "input[type=text], input[type=password]{width:100%;padding:8px;margin-bottom:15px;background:#3d3d3d;border:1px solid #555;color:#e0e0e0;border-radius:3px;}";
        html += ".btn{background:#0088cc;color:white;border:none;padding:8px 15px;margin-right:10px;border-radius:4px;cursor:pointer;}";
        html += ".btn:hover{background:#006699;}";
        html += ".note{background:#3a3a3a;padding:10px;margin-top:15px;border-left:3px solid #0099ff;font-size:0.9em;}";
        html += "</style></head><body>";
        html += "<h1>Zello Configuration</h1>";

        html += "<div class='form-box'>";
        html += "<form method='POST' action='/config/zello/save' accept-charset='UTF-8'>"; // Add accept-charset attribute
        
        // Zello username - use simple attribute escaping for HTML
        String safeUsername = zelloUsername;
        safeUsername.replace("&", "&amp;");
        safeUsername.replace("\"", "&quot;");
        safeUsername.replace("<", "&lt;");
        safeUsername.replace(">", "&gt;");
        
        html += "<label for='username'>Zello Username:</label>";
        html += "<input type='text' id='username' name='username' value=\"" + safeUsername + "\">";
        
        // Zello password
        html += "<label for='password'>Zello Password:</label>";
        html += "<input type='password' id='password' name='password' value=\"" + zelloPassword + "\">";
        
        // Zello channel - use simple attribute escaping for HTML
        String safeChannel = zelloChannel;
        safeChannel.replace("&", "&amp;");
        safeChannel.replace("\"", "&quot;");
        safeChannel.replace("<", "&lt;");
        safeChannel.replace(">", "&gt;");
        
        html += "<label for='channel'>Zello Channel:</label>";
        html += "<input type='text' id='channel' name='channel' value=\"" + safeChannel + "\">";
        
        // Zello API token
        html += "<label for='token'>Zello API Token:</label>";
        html += "<input type='text' id='token' name='token' value=\"" + token + "\">";
        
        html += "<button type='submit' class='btn'>Save Configuration</button>";
        html += "<button type='button' class='btn' onclick=\"window.location.href='/'\">Cancel</button>";
        html += "</form>";
        
        // Add UTF-8 debug info
        html += "<div class='note'>";
        html += "<p><strong>Note:</strong> After saving, the device will reconnect to Zello using the new credentials.</p>";
        html += "<p><strong>Current UTF-8 values:</strong></p>";
        
        // Display username with hex bytes
        html += "<p>Username: <span style='background:#222;padding:2px 5px;'>" + safeUsername + "</span> (Hex: ";
        for (int i = 0; i < zelloUsername.length(); i++) {
            html += String((uint8_t)zelloUsername[i], HEX) + " ";
        }
        html += ")</p>";
        
        // Display channel with hex bytes
        html += "<p>Channel: <span style='background:#222;padding:2px 5px;'>" + safeChannel + "</span> (Hex: ";
        for (int i = 0; i < zelloChannel.length(); i++) {
            html += String((uint8_t)zelloChannel[i], HEX) + " ";
        }
        html += ")</p>";
        html += "</div>";
        
        html += "<p><a href='/'>&larr; Back to Dashboard</a></p>";
        html += "</body></html>";
        server.send(200, "text/html", html);
    });

    // Handle saving Zello configuration - improved UTF-8 handling
    server.on("/config/zello/save", HTTP_POST, []() { 
        bool needReconnect = false;
        
        Serial.println("Received Zello configuration update:");
        
        // Get form values
        if (server.hasArg("username")) {
            String newUsername = server.arg("username");
            Serial.print("Username: ");
            Serial.println(newUsername);
            
            // Debug UTF-8 bytes
            Serial.print("Username UTF-8 bytes: ");
            for (int i = 0; i < newUsername.length(); i++) {
                Serial.printf("%02X ", (uint8_t)newUsername[i]);
            }
            Serial.println();
            
            if (newUsername != zelloUsername) {
                zelloUsername = newUsername;
                needReconnect = true;
            }
        }
        
        if (server.hasArg("password")) {
            String newPassword = server.arg("password");
            if (newPassword != zelloPassword) {
                zelloPassword = newPassword;
                needReconnect = true;
            }
        }
        
        if (server.hasArg("channel")) {
            String newChannel = server.arg("channel");
            Serial.print("Channel: ");
            Serial.println(newChannel);
            
            // Debug UTF-8 bytes
            Serial.print("Channel UTF-8 bytes: ");
            for (int i = 0; i < newChannel.length(); i++) {
                Serial.printf("%02X ", (uint8_t)newChannel[i]);
            }
            Serial.println();
            
            if (newChannel != zelloChannel) {
                zelloChannel = newChannel;
                needReconnect = true;
            }
        }
        
        if (server.hasArg("token") && server.arg("token") != token) {
            token = server.arg("token");
            needReconnect = true;
        }
        
        // Save to file if changes were made
        if (needReconnect) {
            // Save to wifi_credentials.ini
            File wifiFile = SPIFFS.open("/wifi_credentials.ini", "r");
            String fileContent = "";
            
            if (wifiFile) {
                while (wifiFile.available()) {
                    fileContent += wifiFile.readStringUntil('\n') + "\n";
                }
                wifiFile.close();
                
                // Update or add Zello credentials
                bool usernameFound = false;
                bool zelloPasswordFound = false;
                bool channelFound = false;
                
                // Create a temporary buffer for the new content
                String newContent = "";
                int startPos = 0;
                int endPos = 0;
                
                while ((endPos = fileContent.indexOf('\n', startPos)) != -1) {
                    String line = fileContent.substring(startPos, endPos + 1);
                    if (line.indexOf("username=") == 0) {
                        newContent += "username=" + zelloUsername + "\n";
                        usernameFound = true;
                    } 
                    else if (line.indexOf("password_zello=") == 0) {
                        newContent += "password_zello=" + zelloPassword + "\n";
                        zelloPasswordFound = true;
                    } 
                    else if (line.indexOf("channel=") == 0) {
                        newContent += "channel=" + zelloChannel + "\n";
                        channelFound = true;
                    }
                    else {
                        newContent += line;
                    }
                    startPos = endPos + 1;
                }
                
                // Add any missing entries
                if (!usernameFound) {
                    newContent += "username=" + zelloUsername + "\n";
                }
                if (!zelloPasswordFound) {
                    newContent += "password_zello=" + zelloPassword + "\n";
                }
                if (!channelFound) {
                    newContent += "channel=" + zelloChannel + "\n";
                }
                
                // Write the updated content back
                wifiFile = SPIFFS.open("/wifi_credentials.ini", "w");
                if (wifiFile) {
                    wifiFile.print(newContent);
                    wifiFile.close();
                    Serial.println("Zello credentials updated in wifi_credentials.ini");
                }
            }
            
            // Save API token separately
            File tokenFile = SPIFFS.open("/zello-api.key", "w");
            if (tokenFile) {
                tokenFile.print(token);
                tokenFile.close();
                Serial.println("API token updated in zello-api.key");
            }
            
            // Reconnect to WebSocket if needed
            if (needReconnect) {
                Serial.println("Reconnecting to Zello with new credentials...");
                client.close();
                delay(500);
                connectWebSocket();
            }
        }
        
        // Redirect back to the dashboard with a success message
        server.sendHeader("Location", "/");
        server.send(303);
    });

    // ...existing server endpoints...

    // Start the server
    server.begin();
    Serial.println("HTTP server started");
}

// ...remaining existing code...