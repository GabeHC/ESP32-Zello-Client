#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoWebsockets.h>
#include <FS.h>
#include <SPIFFS.h>
#include <AudioOutputI2S.h>
#include <AudioGeneratorOpus.h>
#include "AC101.h"
#include "opus_handler.h"
#include "AudioFileSourceBuffer.h" // Include the new header file
#include <WebServer.h> // Include the WebServer library
#include <Update.h>  // Add this for OTA functionality
#include <mbedtls/base64.h> // Add this include for base64 decoding

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

static uint8_t volume = 5;
const uint8_t volume_step = 2;

using namespace websockets;

const char* updateHTML = R"(
<!DOCTYPE html>
<html>
<head>
    <title>ESP32 OTA Update</title>
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <style>
        body { font-family: Arial; margin: 20px; }
        .progress { width: 100%; background-color: #f0f0f0; padding: 3px; border-radius: 3px; box-shadow: inset 0 1px 3px rgba(0, 0, 0, .2); }
        .progress-bar { width: 0%; height: 20px; background-color: #4CAF50; border-radius: 3px; transition: width 500ms; }
        .button { background-color: #4CAF50; border: none; color: white; padding: 15px 32px; text-align: center; display: inline-block; font-size: 16px; margin: 4px 2px; cursor: pointer; }
    </style>
</head>
<body>
    <h2>ESP32 Firmware Update</h2>
    <div id="upload-status"></div>
    <form method='POST' action='#' enctype='multipart/form-data' id='upload_form'>
        <input type='file' name='update' accept='.bin'>
        <input type='submit' value='Update Firmware' class='button'>
    </form>
    <div class="progress">
        <div class="progress-bar" id="prg"></div>
    </div>
    <script>
        var form = document.getElementById('upload_form');
        var progressBar = document.getElementById('prg');
        var statusDiv = document.getElementById('upload-status');
        
        form.onsubmit = function(e) {
            e.preventDefault();
            var data = new FormData(form);
            var xhr = new XMLHttpRequest();
            xhr.open('POST', '/update', true);
            
            xhr.upload.onprogress = function(e) {
                if (e.lengthComputable) {
                    var percent = (e.loaded / e.total) * 100;
                    progressBar.style.width = percent + '%';
                }
            };
            
            xhr.onreadystatechange = function() {
                if (xhr.readyState === 4) {
                    if (xhr.status === 200) {
                        statusDiv.innerHTML = 'Update Success! Rebooting...';
                    } else {
                        statusDiv.innerHTML = 'Update Failed!';
                    }
                }
            };
            
            xhr.send(data);
        };
    </script>
</body>
</html>
)";

// Forward declarations and structures (add at the top after includes)
class AC101;
class AudioOutputI2S;
class OpusHandler;
class AudioFileSourceBuffer;
class WebServer;

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
AC101 ac101;
AudioOutputI2S* outI2S = nullptr;
OpusHandler* opusHandler = nullptr;
AudioFileSourceBuffer* fileSource = nullptr;
WebServer server(8080);

// Add global variables for dynamic buffer
uint8_t* opusDataBuffer = nullptr;
size_t opusDataLen = 0;

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

// Add this structure to help manage OPUS frames
struct OpusFrame {
    uint8_t toc;          // Table of contents byte
    uint16_t frameSize;   // Size of frame data
    const uint8_t* data;  // Pointer to frame data
};

// Function declarations
void onMessageCallback(WebsocketsMessage message);
void onEventsCallback(WebsocketsEvent event, String data);
void readCredentials();
void setupOTAWebServer();  // Forward declaration
void processOpusPlayback();
void DumpAudioFileSourceBufferState();
OpusPacket findNextOpusPacket(const uint8_t* data, size_t len);

// New function to validate and extract OPUS packet
OpusPacket findNextOpusPacket(const uint8_t* data, size_t len) {
    OpusPacket packet = {nullptr, 0, 6, 20};
    
    // Need minimum 10 bytes (9 header + at least 1 data byte)
    if (len < 10) {
        Serial.println("Packet too short");
        return packet;
    }

    // Debug first bytes
    if (binaryPacketCount < DETAILED_PACKET_COUNT) {
        Serial.print("Header (9 bytes): ");
        for (size_t i = 0; i < 9; i++) {
            Serial.printf("%02X ", data[i]);
        }
        Serial.println();
        
        Serial.print("Frame data starts with: ");
        for (size_t i = 9; i < min(len, (size_t)25); i++) {
            Serial.printf("%02X ", data[i]);
        }
        Serial.println();
    }

    // Treat everything after first 9 bytes as OPUS frame
    packet.data = &data[9];
    packet.length = len - 9;
    
    if (binaryPacketCount < DETAILED_PACKET_COUNT) {
        Serial.printf("Extracted frame: offset=9 size=%d\n", packet.length);
    }

    return packet;
}

// Function to dump the state of AudioFileSourceBuffer
void DumpAudioFileSourceBufferState() {
    Serial.println("AudioFileSourceBuffer State:");
    Serial.print("  opusDataBuffer != nullptr: ");
    Serial.println(opusDataBuffer != nullptr);
    Serial.print("  opusDataLen: ");
    Serial.println(opusDataLen);
    Serial.print("  fileSource->isOpen(): ");
    Serial.println(fileSource->isOpen());
    Serial.print("  fileSource->tell(): ");
    Serial.println(fileSource->tell());
    if (opusDataBuffer != nullptr) {
        Serial.print("  First byte of opusDataBuffer: ");
        Serial.println(opusDataBuffer[0], HEX);
    }
}

// Then your existing processOpusPlayback() function follows
void processOpusPlayback() {
    if (!opusHandler || !isValidAudioStream) return;

    if (!opusHandler->isRunning()) {
        if (opusDataLen >= MIN_BUFFER_SIZE) {
            // Start OPUS decoder
            if (!opusHandler->begin()) {  // Remove nullptr argument
                Serial.println("OPUS begin failed!");
                DumpAudioFileSourceBufferState();
                return;
            }
            opusHandler->processAudioData(opusDataBuffer, opusDataLen);
        }
    }
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
        size_t msgLen = message.length();
        
        // Create a byte array to store the message
        uint8_t rawData[msgLen];
        memcpy(rawData, message.c_str(), msgLen);

        // Print raw data immediately before any processing
        if (binaryPacketCount < DETAILED_PACKET_COUNT) {
            Serial.printf("\n=== Raw Binary Packet %d of %d ===\n", 
                binaryPacketCount + 1, DETAILED_PACKET_COUNT);
            Serial.printf("Raw Length: %u bytes\n", msgLen);
            
            // Print first 32 bytes in hex
            Serial.print("Raw Data: ");
            for (size_t i = 0; i < min(msgLen, (size_t)32); i++) {
                Serial.printf("%02X ", rawData[i]);
                if (i == 8) Serial.print("| "); // Separator after header
            }
            Serial.println();
        }

        // Validate packet type (should be 0x01 for audio)
        if (rawData[0] != 0x01) {
            Serial.printf("Invalid packet type: 0x%02X\n", rawData[0]);
            binaryPacketCount++;
            return;
        }

        // Extract header information for audio processing
        uint32_t streamId = (rawData[1] << 24) | (rawData[2] << 16) | 
                           (rawData[3] << 8) | rawData[4];
        uint32_t packetId = (rawData[5] << 24) | (rawData[6] << 16) | 
                           (rawData[7] << 8) | rawData[8];

        // Get Opus data
        const uint8_t* opusData = rawData + 9;
        size_t opusLen = msgLen - 9;

        // Debug first few packets
        if (binaryPacketCount < DETAILED_PACKET_COUNT) {
            Serial.printf("\nAudio Packet %d:\n", binaryPacketCount);
            Serial.printf("Stream ID: %u\n", streamId);
            Serial.printf("Packet ID: %u\n", packetId);
            Serial.printf("Opus Length: %u\n", opusLen);
            Serial.print("Opus data: ");
            for (int i = 0; i < min(16, (int)opusLen); i++) {
                Serial.printf("%02X ", opusData[i]);
            }
            Serial.println();
        }

        // Process the Opus data
        if (binaryPacketCount == 0) {
            // First packet handling
            if (opusDataBuffer) {
                free(opusDataBuffer);
                opusDataBuffer = nullptr;
            }
            opusDataBuffer = (uint8_t*)malloc(OPUS_BUFFER_SIZE);  // Use fixed size buffer
            if (opusDataBuffer) {
                memcpy(opusDataBuffer, opusData, opusLen);
                opusDataLen = opusLen;
            }
        } else {
            // Subsequent packets handling
            if (opusDataBuffer && (opusDataLen + opusLen) <= OPUS_BUFFER_SIZE) {
                memcpy(opusDataBuffer + opusDataLen, opusData, opusLen);
                opusDataLen += opusLen;
            }
        }

        binaryPacketCount++;
        totalBytesReceived += opusLen;
        totalPacketsReceived++;

    } else {
        String msg = message.data();
        
        // Print full message for stream start/stop
        if (msg.indexOf("\"command\":\"on_stream_start\"") >= 0) {
            Serial.println("\n=== Stream Start Message ===");
            Serial.println(msg);
            Serial.println("===========================\n");
            // Extract codec_header
            int headerStart = msg.indexOf("\"codec_header\":\"");
            if (headerStart >= 0) {
                headerStart += 15;
                int headerEnd = msg.indexOf("\"", headerStart);
                if (headerEnd > headerStart) {
                    String codecHeader = msg.substring(headerStart, headerEnd);
                    Serial.printf("Codec Header: %s\n", codecHeader.c_str());
                    
                    // Parse base64 codec header
                    size_t decodedLen;
                    uint8_t decoded[4];
                    mbedtls_base64_decode(decoded, 4, &decodedLen, 
                        (uint8_t*)codecHeader.c_str(), codecHeader.length());
                    
                    if (decodedLen == 4) {
                        OpusConfig config;
                        config.sampleRate = decoded[0] | (decoded[1] << 8);
                        config.framesPerPacket = decoded[2];
                        config.frameSizeMs = decoded[3];
                        
                        Serial.printf("Opus Config: %dHz, %d frames/packet, %dms/frame\n",
                            config.sampleRate, config.framesPerPacket, config.frameSizeMs);
                            
                        // Configure audio hardware
                        AC101::I2sSampleRate_t sampleRate;
                        switch (config.sampleRate) {
                            case 48000:
                                sampleRate = AC101::SAMPLE_RATE_48000;
                                break;
                            case 44100:
                                sampleRate = AC101::SAMPLE_RATE_44100;
                                break;
                            case 16000:
                                sampleRate = AC101::SAMPLE_RATE_16000;
                                break;
                            default:
                                Serial.printf("Unsupported sample rate: %d\n", config.sampleRate);
                                sampleRate = AC101::SAMPLE_RATE_48000;
                        }
                        ac101.SetI2sSampleRate(sampleRate);
                    }
                }
            }
            
            // Reset stream state
            streamStartTime = millis();
            totalBytesReceived = 0;
            totalPacketsReceived = 0;
            binaryPacketCount = 0;
            isValidAudioStream = true;
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
            if (opusHandler) {
                opusHandler->stop();
            }
            if (opusDataBuffer) {
                free(opusDataBuffer);
                opusDataBuffer = nullptr;
                opusDataLen = 0;
            }
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
        sprintf(logon, "{\"command\": \"logon\",\"seq\": 1,\"auth_token\": \"%s\",\"username\": \"bv5dj-r\",\"password\": \"gabpas\",\"channel\": \"黑川家\"}", token.c_str());
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
        server.sendHeader("Connection", "close");
        server.send(200, "text/html", updateHTML);
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
    
    // Wire needed for AC101
    Wire.begin(IIC_DATA, IIC_CLK);       // Or your SDA, SCL pins
    Wire.setClock(100000);    // 100 kHz I2C

    if (!SPIFFS.begin(true)) {
        Serial.println("Failed to mount SPIFFS");
        return;
    }

    // Read credentials from SPIFFS
    readCredentials();

    // Check credentials before connecting to WiFi
    if (ssid.length() == 0 || password.length() == 0) {
        Serial.println("SSID or password is missing!");
        return;
    }

    WiFi.begin(ssid.c_str(), password.c_str());
    WiFi.hostname("ESP32-Audio");  // Add this line

    // Wait for WiFi connection
    Serial.print("Connecting to WiFi");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    
    // Make IP address very visible in Serial Monitor
    Serial.println("\n========================================");
    Serial.println("         WiFi Connected!");
    Serial.print("         IP: ");
    Serial.println(WiFi.localIP());
    Serial.println("========================================\n");

    // Read the CA certificate from SPIFFS
    File caFile = SPIFFS.open("/zello-io.crt", "r");
    if (!caFile) {
        Serial.println("Failed to open CA file");
        return;
    }
    String caCert = caFile.readString();
    caCert.trim(); // Remove whitespace (\r, \n, etc.)
    caFile.close();

    // Set the SSL CA certificate
    client.setCACert(caCert.c_str());

    // Set WebSocket callbacks
    client.onMessage(onMessageCallback);
    client.onEvent(onEventsCallback);

    // Connect to WebSocket server
    Serial.println("Connecting to WebSocket server...");
    client.connect(websocket_server);

    // Add AC101 initialization
    Serial.printf("Connect to AC101 codec... ");
    AC101 ac101; // Declare ac101 object here
    bool ac101_ok = false;
    for (int i = 0; i < 5; i++) {
        Wire.flush();
        delay(100);
        ac101_ok = ac101.begin(IIC_DATA, IIC_CLK);
        if (ac101_ok) break;
        Serial.printf("Failed! Retry %d\n", i + 1);
        delay(500);
    }
    if (!ac101_ok) {
        Serial.println("Failed to initialize AC101 codec!");
        return;
    }
    Serial.println("OK");

    // Configure AC101
    ac101.SetVolumeSpeaker(volume);
    ac101.SetVolumeHeadphone(volume);
    ac101.SetMode(AC101::MODE_ADC_DAC);
    ac101.SetI2sSampleRate(AC101::SAMPLE_RATE_48000);
    ac101.SetI2sClock(AC101::BCLK_DIV_16, false, AC101::LRCK_DIV_32, false);
    ac101.SetI2sMode(AC101::MODE_SLAVE);
    ac101.SetI2sWordSize(AC101::WORD_SIZE_16_BITS);
    ac101.SetI2sFormat(AC101::DATA_FORMAT_I2S);

    // Enable amplifier
    pinMode(GPIO_PA_EN, OUTPUT);
    digitalWrite(GPIO_PA_EN, HIGH);

    outI2S = new AudioOutputI2S();
    outI2S->SetPinout(IIS_SCLK, IIS_LCLK, IIS_DSIN);
    outI2S->SetBitsPerSample(16);
    outI2S->SetRate(48000);
    outI2S->SetChannels(1);
    outI2S->begin();

    // Create OPUS decoder
    opusHandler = new OpusHandler(outI2S);

    // Initialize AudioFileSourceBuffer
    fileSource = new AudioFileSourceBuffer();

    // Setup OTA Web Server
    setupOTAWebServer();
    Serial.println("HTTP server started");
    Serial.print("Update interface available at http://");
    Serial.println(WiFi.localIP());

    Serial.println("Setup complete");
}

void loop() {
    server.handleClient();
    static unsigned long lastProcess = 0;
    unsigned long now = millis();
    
    client.poll();  // Handle WebSocket messages
    
    // Process OPUS every 5ms
    if (now - lastProcess >= 5) {
        processOpusPlayback();
        lastProcess = now;
    }

    // If we're running OPUS, call loop repeatedly
    if (opusHandler && opusHandler->isRunning()) {
        if (!opusHandler->loop()) {
            opusHandler->stop();
        }
    }

    // Handle OTA Web Server
    server.handleClient();

    // Other tasks...
}