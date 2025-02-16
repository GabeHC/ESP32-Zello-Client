#include "opus_handler.h"
#include <mbedtls/base64.h>
#include <Arduino.h>
#include <cstring>

OpusHandler::OpusHandler(AudioOutputI2S* out) : 
    output(out), 
    opus(nullptr),
    fileSource(nullptr),
    running(false),
    buffer(nullptr),
    bufferSize(0),
    bufferPos(0) {
}

OpusHandler::~OpusHandler() {
    stop();
    if (opus) {
        delete opus;
        opus = nullptr;
    }
    if (fileSource) {
        delete fileSource;
        fileSource = nullptr;
    }
    if (buffer) {
        free(buffer);
        buffer = nullptr;
    }
}

bool OpusHandler::createOpusHeader(uint8_t* header) {
    // Magic signature "OpusHead"
    memcpy(header, OPUS_HEADER_MAGIC, 8);
    
    // Version (1 byte)
    header[8] = OPUS_VERSION;
    
    // Channel count (1 byte)
    header[9] = OPUS_CHANNELS;
    
    // Pre-skip (2 bytes, little endian)
    header[10] = OPUS_PRESKIP & 0xFF;
    header[11] = (OPUS_PRESKIP >> 8) & 0xFF;
    
    // Sample rate (4 bytes, little endian)
    header[12] = OPUS_SAMPLE_RATE & 0xFF;
    header[13] = (OPUS_SAMPLE_RATE >> 8) & 0xFF;
    header[14] = (OPUS_SAMPLE_RATE >> 16) & 0xFF;
    header[15] = (OPUS_SAMPLE_RATE >> 24) & 0xFF;
    
    // Output gain (2 bytes, little endian)
    header[16] = OPUS_GAIN & 0xFF;
    header[17] = (OPUS_GAIN >> 8) & 0xFF;
    
    // Channel mapping family (1 byte)
    header[18] = 0; // RTP mapping

    return true;
}

bool OpusHandler::begin() {
    if (!output || bufferSize == 0) return false;

    // Create decoder if needed
    if (!opus) {
        opus = new AudioGeneratorOpus();
    }

    // Setup output
    output->SetBitsPerSample(16);
    output->SetChannels(OPUS_CHANNELS);
    output->SetRate(OPUS_SAMPLE_RATE);

    // Create formatted stream with both headers
    uint8_t opusHeader[OPUS_HEADER_SIZE];
    createOpusHeader(opusHeader);

    // Debug the original data
    Serial.println("Original Data First 16 bytes:");
    for (int i = 0; i < min(16, (int)bufferSize); i++) {
        Serial.printf("%02X ", buffer[i]);
    }
    Serial.println();

    // Create new buffer with header and tags
    size_t totalSize = OPUS_HEADER_SIZE + bufferSize;
    uint8_t* streamBuffer = (uint8_t*)malloc(totalSize);
    if (!streamBuffer) {
        Serial.println("Failed to allocate memory for stream");
        return false;
    }

    // Copy headers and data
    memcpy(streamBuffer, opusHeader, OPUS_HEADER_SIZE);
    memcpy(streamBuffer + OPUS_HEADER_SIZE, buffer, bufferSize);

    // Debug the final stream
    Serial.println("Final Stream First 32 bytes:");
    for (int i = 0; i < min(32, (int)totalSize); i++) {
        Serial.printf("%02X ", streamBuffer[i]);
    }
    Serial.println();

    // Create or reset file source
    if (fileSource) {
        delete fileSource;
    }
    fileSource = new AudioFileSourceMemory();

    // Open the memory source with our formatted stream
    if (!fileSource->open(streamBuffer, totalSize)) {
        Serial.println("Failed to open memory source");
        free(streamBuffer);
        return false;
    }

    // Initialize decoder
    if (!opus->begin(fileSource, output)) {
        Serial.println("Failed to initialize OPUS decoder");
        free(streamBuffer);
        return false;
    }

    // Only free the temporary buffer after successful initialization
    free(streamBuffer);
    running = true;
    return true;
}

bool OpusHandler::processAudioData(const uint8_t* data, size_t len) {
    if (!data || len == 0) return false;
    
    // Allocate or resize buffer
    uint8_t* newBuffer = (uint8_t*)realloc(buffer, bufferSize + len);
    if (!newBuffer) {
        Serial.println("Failed to allocate memory for audio data");
        return false;
    }

    // Copy new data
    memcpy(newBuffer + bufferSize, data, len);
    buffer = newBuffer;
    bufferSize += len;

    return true;
}

size_t OpusHandler::read(uint8_t* dest, size_t length) {
    if (!buffer || bufferPos >= bufferSize) return 0;
    
    size_t available = bufferSize - bufferPos;
    size_t toRead = min(length, available);
    
    memcpy(dest, buffer + bufferPos, toRead);
    bufferPos += toRead;
    
    return toRead;
}

bool OpusHandler::seek(size_t pos) {
    if (pos > bufferSize) return false;
    bufferPos = pos;
    return true;
}

bool OpusHandler::isRunning() {
    return running && opus && opus->isRunning();
}

void OpusHandler::stop() {
    if (opus) {
        opus->stop();
    }
    running = false;
}

bool OpusHandler::loop() {
    if (!opus || !running) return false;
    return opus->loop();
}

// Add Base64 decoding function
uint8_t* decodeBase64(const char* input, size_t* outLen) {
    if (!input || !outLen) {
        Serial.println("Invalid input parameters");
        return nullptr;
    }

    size_t len = strlen(input);
    size_t outputLen;
    
    // Calculate required buffer size
    int ret = mbedtls_base64_decode(nullptr, 0, &outputLen, 
                                   (const unsigned char*)input, len);
    if (ret != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
        Serial.printf("Base64 size calculation failed: %d\n", ret);
        return nullptr;
    }
    
    uint8_t* output = (uint8_t*)malloc(outputLen);
    if (!output) {
        Serial.println("Failed to allocate memory for decoded data");
        return nullptr;
    }
    
    // Actual decoding
    ret = mbedtls_base64_decode(output, outputLen, outLen, 
                               (const unsigned char*)input, len);
    if (ret != 0) {
        Serial.printf("Base64 decode failed with error: %d\n", ret);
        free(output);
        return nullptr;
    }

    Serial.printf("Successfully decoded %d bytes\n", *outLen);
    return output;
}

// Example function if you need it
void DumpOPUSState() {
    extern AudioGeneratorOpus* opus; // Access the global opus instance
    extern size_t opusDataLen;

    if (!opus) {
        Serial.println("No OPUS object");
        return;
    }
    Serial.printf("OPUS isRunning: %s\n", opus->isRunning() ? "Yes" : "No");
    Serial.printf("Buffer size: %d bytes\n", opusDataLen);
}

bool initOpusDecoder(uint8_t* header, size_t headerLen) {
    extern AudioOutputI2S* outI2S;

    // Configure opus settings before begin()
    if (outI2S) {
        outI2S->SetBitsPerSample(16);
        outI2S->SetChannels(1);
        outI2S->SetRate(48000);
    }
    // Pass header to OPUS decoder (if needed)
    // For now, just print the header info
    Serial.printf("initOpusDecoder: Header length = %d\n", headerLen);
    return true;
}