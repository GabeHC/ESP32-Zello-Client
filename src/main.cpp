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
#include "AudioFileSourceBuffer.h"
#include <WebServer.h>
#include <Update.h>

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

static uint8_t volume = 5;
const uint8_t volume_step = 2;

using namespace websockets;

WebServer server(8080);

// Add near the top of the file, after includes but before other declarations
// Remove these lines
// CircularBuffer<uint8_t, OPUS_BUFFER_SIZE> opusBufferCB;
// AudioGeneratorOpus* opus = nullptr;
// AudioFileSourceBuffer* fileSource = nullptr;

// Add this declaration
OpusHandler* opusHandler = nullptr;

AudioOutputI2S* outI2S = nullptr;
AudioFileSourceBuffer* fileSource = nullptr;

bool isValidAudioStream = false;
int binaryPacketCount = 0;
const int DETAILED_PACKET_COUNT = 4;

// Add global variables for dynamic buffer
uint8_t* opusDataBuffer = nullptr;
size_t opusDataLen = 0;

// Add these global variables near the top with other declarations
unsigned long streamStartTime = 0;
unsigned long streamDuration = 0;
size_t totalBytesReceived = 0;
int totalPacketsReceived = 0;

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

// Add these helper functions at the top of the file
struct OpusPacket {
    const uint8_t* data;
    size_t length;
    uint8_t frameCount;    // Should be 6
    uint8_t frameDuration; // Should be 20ms
};

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

// Add this function and call it in setup() after SPIFFS.begin()
void listFiles() {
    Serial.println("\nSPIFFS files:");
    File root = SPIFFS.open("/");
    File file = root.openNextFile();
    while(file) {
        Serial.printf("- %s, size: %d bytes\n", file.name(), file.size());
        file = root.openNextFile();
    }
    Serial.println("");
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

// Update the binary message handler
void onMessageCallback(WebsocketsMessage message) {
    if (message.isBinary()) {
        if (!isValidAudioStream) return;

        size_t msgLen = message.length();
        const uint8_t* data = (const uint8_t*)message.data().c_str();
        
        // Skip first 8 bytes of WebSocket framing
        const uint8_t* opusData = data + 8;
        size_t opusLen = msgLen - 8;

        // Debug first packet extensively
        if (binaryPacketCount < DETAILED_PACKET_COUNT) {
            Serial.printf("\n=== Binary Packet %d ===\n", binaryPacketCount);
            Serial.printf("Raw message length: %d\n", msgLen);
            Serial.print("WebSocket header: ");
            for (int i = 0; i < 8; i++) {
                Serial.printf("%02X ", data[i]);
            }
            Serial.println();
            Serial.print("OPUS data: ");
            for (int i = 0; i < min(16, (int)opusLen); i++) {
                Serial.printf("%02X ", opusData[i]);
            }
            Serial.println();
        }

        // First packet: initialize buffer
        if (binaryPacketCount == 0) {
            if (opusDataBuffer) {
                free(opusDataBuffer);
                opusDataBuffer = nullptr;
            }
            
            // Allocate new buffer and copy OPUS data
            opusDataBuffer = (uint8_t*)malloc(opusLen);
            if (opusDataBuffer) {
                memcpy(opusDataBuffer, opusData, opusLen);
                opusDataLen = opusLen;
            }
        } else {
            // Calculate new size needed
            size_t newSize = opusDataLen + opusLen;
            
            if (opusDataBuffer) {
                uint8_t* newBuffer = (uint8_t*)malloc(newSize);
                if (newBuffer) {
                    memcpy(newBuffer, opusDataBuffer, opusDataLen);
                    memcpy(newBuffer + opusDataLen, opusData, opusLen);
                    free(opusDataBuffer);
                    opusDataBuffer = newBuffer;
                    opusDataLen = newSize;
                }
            }
        }

        if (binaryPacketCount < DETAILED_PACKET_COUNT) {
            Serial.printf("Total buffer size: %d bytes\n", opusDataLen);
            if (opusDataBuffer) {
                Serial.print("Buffer starts with: ");
                for (int i = 0; i < min(16, (int)opusDataLen); i++) {
                    Serial.printf("%02X ", opusDataBuffer[i]);
                }
                Serial.println();
            }
        }

        binaryPacketCount++;
        totalBytesReceived += opusLen;
        totalPacketsReceived++;
    } else {
        String msg = message.data();
        
        if (msg.indexOf("\"command\":\"on_stream_start\"") >= 0) {
            // Reset everything
            streamStartTime = millis();
            totalBytesReceived = 0;
            totalPacketsReceived = 0;
            binaryPacketCount = 0;
            isValidAudioStream = true;

            // Get packet_duration from stream start
            int durationStart = msg.indexOf("\"packet_duration\":") + 17;
            int durationEnd = msg.indexOf(",", durationStart);
            if (durationStart > 17 && durationEnd > durationStart) {
                String duration = msg.substring(durationStart, durationEnd);
                Serial.printf("Packet duration: %s ms\n", duration.c_str());
            }

            // Reset buffer
            if (opusDataBuffer) {
                free(opusDataBuffer);
                opusDataBuffer = nullptr;
            }
            opusDataLen = 0;

            Serial.println("\n=== Stream Started ===");
            Serial.printf("Time: %lu ms\n", streamStartTime);

        } else if (msg.indexOf("\"command\":\"on_stream_stop\"") >= 0) {
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
        if (!SPIFFS.exists("/ota_update.html")) {
            Serial.println("OTA update file not found!");
            server.send(404, "text/plain", "OTA update file not found!");
            return;
        }
        
        File file = SPIFFS.open("/ota_update.html", "r");
        if (!file) {
            Serial.println("Failed to open OTA update file");
            server.send(500, "text/plain", "Failed to open OTA update file");
            return;
        }
        
        server.streamFile(file, "text/html");
        file.close();
        Serial.println("OTA page served successfully");
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
    listFiles();  // Add this line to see what files are actually in SPIFFS

    // List files in SPIFFS
    listFiles();

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