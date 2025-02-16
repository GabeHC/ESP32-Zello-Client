/*    
	AC101 Codec driver library example.
	Uses the ESP32-A1S module with integrated AC101 codec, mounted on the ESP32 Audio Kit board:
	https://wiki.ai-thinker.com/esp32-audio-kit

	Required library: ESP8266Audio https://github.com/earlephilhower/ESP8266Audio

	Copyright (C) 2019, Ivo Pullens, Emmission
	
	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoWebsockets.h>
#include <FS.h>
#include <SPIFFS.h>
#include <AudioFileSourceHTTPStream.h>
#include <AudioGeneratorOpus.h>
#include <AudioOutputI2S.h>
#include <AC101.h>
#include <Wire.h>
#include "AudioFileSourcePROGMEM.h"
#include "AudioGeneratorRTTTL.h"

using namespace websockets;

String ssid;
String password;
String token;
const char* websocket_server = "wss://zello.io/ws"; // Use wss:// for secure WebSocket

bool LED22 = false;
bool LED19 = false;
WebsocketsClient client;

AudioGeneratorOpus *opus = nullptr;
AudioFileSourceHTTPStream *file = nullptr;
AudioOutputI2S *out = nullptr;

AC101 ac101;

const char song[] PROGMEM = 
"Batman:d=8,o=5,b=180:d,d,c#,c#,c,c,c#,c#,d,d,c#,c#,c,c,c#,c#,d,d#,c,c#,c,c,c#,c#,f,p,4f";

AudioGeneratorRTTTL *rtttl;
AudioFileSourcePROGMEM *fileProgmem;
AudioOutputI2S *outI2S;

#define IIS_SCLK                    27
#define IIS_LCLK                    26
#define IIS_DSIN                    25

#define IIC_CLK                     32
#define IIC_DATA                    33

#define GPIO_PA_EN                  GPIO_NUM_21
#define GPIO_SEL_PA_EN              GPIO_SEL_21

#define PIN_PLAY                    (23)      // KEY 4
#define PIN_VOL_UP                  (18)      // KEY 5
#define PIN_VOL_DOWN                (5)       // KEY 6

static uint8_t volume = 5;
const uint8_t volume_step = 2;

// Function prototypes
void onMessageCallback(WebsocketsMessage message);
void onEventsCallback(WebsocketsEvent event, String data);
void readCredentials();
void i2cScanner();
bool pressed(const int pin);

void setup() {
    pinMode(22, OUTPUT);
    digitalWrite(22, HIGH);
    pinMode(19, OUTPUT);
    digitalWrite(19, LOW);

    Serial.begin(115200);

    // Initialize SPIFFS
    if (!SPIFFS.begin(true)) {
        Serial.println("An Error has occurred while mounting SPIFFS");
        return;
    }

    // Read credentials from SPIFFS
    readCredentials();

    // Ensure credentials are read before connecting to WiFi
    if (ssid.length() == 0 || password.length() == 0) {
        Serial.println("SSID or password is missing!");
        return;
    }

    WiFi.begin(ssid.c_str(), password.c_str());

    // Wait for WiFi connection
    Serial.print("Connecting to WiFi");
    while (WiFi.status() != WL_CONNECTED) {
        delay(1000);
        Serial.print(".");
    }
    Serial.println("");
    Serial.println("Connected to WiFi");

    // Read the CA certificate from the file
    File caFile = SPIFFS.open("/zello-io.crt", "r");
    if (!caFile) {
        Serial.println("Failed to open CA file");
        return;
    }

    String caCert = caFile.readString();
    caCert.trim(); // Remove any leading/trailing whitespace, including \r and \n
    caFile.close();

    // Set the SSL CA certificate
    client.setCACert(caCert.c_str());

    // Run callback when messages are received
    client.onMessage(onMessageCallback);
    
    // Run callback when events are occurring
    client.onEvent(onEventsCallback);

    // Connect to server
    Serial.println("Connecting to WebSocket server...");
    client.connect(websocket_server);

    // Send a message
    char logon[1024];
    sprintf(logon,"{\"command\": \"logon\",\"seq\": 1,\"auth_token\": \"%s\",\"username\": \"bv5dj-r\",\"password\": \"gabpas\",\"channel\": \"ZELLO無線聯合網\"}", token.c_str());
    client.send(logon);

    // Send a ping
    client.ping();

    // Initialize I2C
    Wire.begin(IIC_DATA, IIC_CLK); // SDA, SCL

    // Scan I2C bus
    i2cScanner();

    // Initialize AC101 codec
    Serial.printf("Connect to AC101 codec... ");
    while (not ac101.begin(IIC_DATA, IIC_CLK)) {
        Serial.printf("Failed!\n");
        delay(1000);
    }
    Serial.printf("OK\n");

    ac101.SetVolumeSpeaker(volume);
    ac101.SetVolumeHeadphone(volume);

    // Enable amplifier
    pinMode(GPIO_PA_EN, OUTPUT);
    digitalWrite(GPIO_PA_EN, HIGH);

    // Configure keys on ESP32 Audio Kit board
    pinMode(PIN_PLAY, INPUT_PULLUP);
    pinMode(PIN_VOL_UP, INPUT_PULLUP);
    pinMode(PIN_VOL_DOWN, INPUT_PULLUP);

    // Create audio source from progmem, enable I2S output,
    // configure I2S pins to match the board and create ringtone generator.
    fileProgmem = new AudioFileSourcePROGMEM(song, strlen_P(song));
    outI2S = new AudioOutputI2S();
    outI2S->SetPinout(IIS_SCLK /*bclkPin*/, IIS_LCLK /*wclkPin*/, IIS_DSIN /*doutPin*/);
    rtttl = new AudioGeneratorRTTTL();

    Serial.printf("Use KEY4 to play, KEY5/KEY6 for volume Up/Down\n");

    Serial.println("Setup complete");
}

void loop() {
    // Maintain WebSocket connection
    client.poll();

    // Decode audio stream
    if (opus && opus->isRunning()) {
        if (!opus->loop()) {
            opus->stop();
        }
    }

    bool updateVolume = false;

    if (pressed(PIN_PLAY) && !rtttl->isRunning()) {
        // Start playing
        fileProgmem->seek(0, SEEK_SET);
        rtttl->begin(fileProgmem, outI2S);
        updateVolume = true;
    }

    if (rtttl->isRunning()) {
        if (!rtttl->loop()) {
            rtttl->stop();
            // Last note seems to loop after stop.
            // To silence also set volume to 0.
            ac101.SetVolumeSpeaker(0);
            ac101.SetVolumeHeadphone(0);
        }
    }

    if (pressed(PIN_VOL_UP)) {
        if (volume <= (63 - volume_step)) {
            // Increase volume
            volume += volume_step;
            updateVolume = true;
        }
    }
    if (pressed(PIN_VOL_DOWN)) {
        if (volume >= volume_step) {
            // Decrease volume
            volume -= volume_step;
            updateVolume = true;
        }
    }
    if (updateVolume) {
        // Volume change requested
        Serial.printf("Volume %d\n", volume);
        ac101.SetVolumeSpeaker(volume);
        ac101.SetVolumeHeadphone(volume);
    }
}

// Function definitions
void onMessageCallback(WebsocketsMessage message) {
    Serial.print("Got Message: ");
    if (message.isBinary()) {       
        Serial.println("Binary message received");
    } else {
        Serial.println(message.data());
        digitalWrite(22, (LED22 = !LED22) ? HIGH : LOW); // Toggle LED22 and set the pin based on the new value
    }
}

void onEventsCallback(WebsocketsEvent event, String data) {
    if(event == WebsocketsEvent::ConnectionOpened) {
        Serial.println("Connection Opened");
    } else if(event == WebsocketsEvent::ConnectionClosed) {
        Serial.println("Connection Closed");
    } else if(event == WebsocketsEvent::GotPing) {
        Serial.println("Got Ping");
        client.pong(); // Send pong in response to ping
        digitalWrite(19, (LED19 = !LED19) ? HIGH : LOW); // Toggle LED19 and set the pin based on the new value
    } else if(event == WebsocketsEvent::GotPong) {
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
        line.trim(); // Remove any leading/trailing whitespace, including \r and \n
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
    token.trim(); // Remove any leading/trailing whitespace, including \r and \n
    tokenFile.close();
}

void i2cScanner() {
    Serial.println("Scanning I2C bus...");
    byte error, address;
    int nDevices = 0;

    for (address = 1; address < 127; address++) {
        Wire.beginTransmission(address);
        error = Wire.endTransmission();

        if (error == 0) {
            Serial.print("I2C device found at address 0x");
            if (address < 16) Serial.print("0");
            Serial.print(address, HEX);
            Serial.println(" !");
            nDevices++;
        } else if (error == 4) {
            Serial.print("Unknown error at address 0x");
            if (address < 16) Serial.print("0");
            Serial.println(address, HEX);
        }
    }

    if (nDevices == 0) {
        Serial.println("No I2C devices found\n");
    } else {
        Serial.println("done\n");
    }
}

bool pressed(const int pin) {
    if (digitalRead(pin) == LOW) {
        delay(500);
        return true;
    }
    return false;
}

