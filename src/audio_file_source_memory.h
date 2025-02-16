#pragma once

#include <AudioFileSource.h>

// OPUS stream constants
#define OPUS_ID_HEADER_SIZE     19
#define OPUS_COMMENT_SIZE       12
#define OPUS_PACKET_SIZE        120  // 120ms frames
#define OPUS_SAMPLE_RATE        48000
#define OPUS_CHANNELS           1
#define OPUS_FRAME_SIZE        960  // 20ms at 48kHz
#define OPUS_SEGMENT_SIZE      255  // Max segment size

class AudioFileSourceMemory : public AudioFileSource {
private:
    uint8_t* buffer;
    size_t bufferSize;
    size_t position;
    bool initialized;
    bool firstPacket;

    void createOpusHeaders(uint8_t* outBuffer, size_t* outSize) {
        uint8_t idHeader[OPUS_ID_HEADER_SIZE] = {
            'O', 'p', 'u', 's', 'H', 'e', 'a', 'd',  // Magic signature
            0x01,                                     // Version
            OPUS_CHANNELS,                           // Channels
            0x00, 0x00,                             // Pre-skip
            0x80, 0xBB, 0x00, 0x00,                 // Sample rate (48000)
            0x00, 0x00,                             // Output gain
            0x00                                     // Channel mapping
        };

        uint8_t commentHeader[OPUS_COMMENT_SIZE] = {
            'O', 'p', 'u', 's', 'T', 'a', 'g', 's',  // Magic signature
            0x00, 0x00, 0x00, 0x00                    // No vendor string
        };

        memcpy(outBuffer, idHeader, OPUS_ID_HEADER_SIZE);
        memcpy(outBuffer + OPUS_ID_HEADER_SIZE, commentHeader, OPUS_COMMENT_SIZE);
        *outSize = OPUS_ID_HEADER_SIZE + OPUS_COMMENT_SIZE;
    }

    uint8_t* formatAudioData(const uint8_t* data, size_t len, size_t* outSize) {
        // Calculate needed space for segmented data
        size_t numSegments = (len + OPUS_SEGMENT_SIZE - 1) / OPUS_SEGMENT_SIZE;
        *outSize = len + numSegments; // Add space for segment sizes

        uint8_t* formatted = (uint8_t*)malloc(*outSize);
        if (!formatted) return nullptr;

        size_t inPos = 0;
        size_t outPos = 0;

        // Format data into segments
        while (inPos < len) {
            size_t remaining = len - inPos;
            uint8_t segmentSize = (remaining > OPUS_SEGMENT_SIZE) ? 
                                 OPUS_SEGMENT_SIZE : remaining;
            
            formatted[outPos++] = segmentSize;  // Write segment size
            memcpy(formatted + outPos, data + inPos, segmentSize);  // Write segment data
            
            outPos += segmentSize;
            inPos += segmentSize;
        }

        return formatted;
    }

    void dumpBuffer(const char* label, const uint8_t* data, size_t len) {
        Serial.printf("\n=== %s (%d bytes) ===\n", label, len);
        for (size_t i = 0; i < len; i++) {
            Serial.printf("%02X ", data[i]);
            if ((i + 1) % 16 == 0) Serial.println();
        }
        Serial.println("\n===================\n");
    }

    void dumpOpusFrames(const uint8_t* data, size_t len) {
        Serial.printf("\n=== OPUS Frame Analysis (Packet: %d bytes) ===\n", len);
        size_t pos = 0;
        int frameCount = 0;
        
        while (pos < len) {
            // Check for TOC byte and ensure we have at least 3 bytes (TOC + size)
            if (data[pos] == 0x80 && (pos + 2) < len) {
                frameCount++;
                // Get frame size from next two bytes (little endian)
                uint16_t frameSize = data[pos + 1] | (data[pos + 2] << 8);
                
                Serial.printf("\nFrame %d at offset %d:\n", frameCount, pos);
                Serial.printf("- TOC: 0x%02X\n", data[pos]);
                Serial.printf("- Size bytes: %02X %02X\n", data[pos + 1], data[pos + 2]);
                Serial.printf("- Frame size: %d bytes\n", frameSize);
                
                // Verify we have enough data for this frame
                if (pos + 3 + frameSize <= len) {
                    Serial.print("- Frame data: ");
                    for (int i = 0; i < min(16, (int)frameSize); i++) {
                        Serial.printf("%02X ", data[pos + 3 + i]);
                    }
                    Serial.println(frameSize > 16 ? "..." : "");
                    
                    // Skip to next frame
                    pos += 3 + frameSize;  // TOC + size(2) + frameSize
                } else {
                    Serial.println("WARNING: Frame extends beyond packet boundary!");
                    break;
                }
            } else {
                pos++;
            }
        }
        
        Serial.printf("\nFound %d OPUS frames\nTotal packet size: %d bytes\n===================\n", 
                     frameCount, len);
    }

public:
    AudioFileSourceMemory() : 
        buffer(nullptr), 
        bufferSize(0), 
        position(0), 
        initialized(false),
        firstPacket(true) {}
    
    virtual ~AudioFileSourceMemory() {
        close();
    }

    bool open(const uint8_t* data, size_t len) {
        close();

        if (firstPacket) {
            // First packet: headers + complete OPUS packet
            buffer = (uint8_t*)malloc(OPUS_ID_HEADER_SIZE + OPUS_COMMENT_SIZE + len);
            if (!buffer) return false;

            size_t headerSize;
            createOpusHeaders(buffer, &headerSize);
            memcpy(buffer + headerSize, data, len);  // Copy entire packet including TOC
            bufferSize = headerSize + len;
            firstPacket = false;
            
            Serial.printf("First packet: %d bytes (headers) + %d bytes (OPUS)\n", 
                         headerSize, len);
        } else {
            // Subsequent packets: just copy the complete OPUS packet
            buffer = (uint8_t*)malloc(len);
            if (!buffer) return false;
            memcpy(buffer, data, len);  // Copy entire packet including TOC
            bufferSize = len;
            
            Serial.printf("OPUS packet: %d bytes\n", len);
        }

        position = 0;
        initialized = true;
        return true;
    }

    virtual bool open(const char* filename) override {
        return false;  // Not used
    }

    virtual uint32_t read(void *data, uint32_t len) override {
        if (!initialized || !buffer || position >= bufferSize) return 0;
        
        size_t available = bufferSize - position;
        size_t toRead = min(len, available);
        
        memcpy(data, buffer + position, toRead);
        position += toRead;
        
        return toRead;
    }

    virtual bool seek(int32_t pos, int dir) override {
        if (!initialized) return false;
        
        size_t newPos;
        switch (dir) {
            case SEEK_SET: newPos = pos; break;
            case SEEK_CUR: newPos = position + pos; break;
            case SEEK_END: newPos = bufferSize + pos; break;
            default: return false;
        }
        
        if (newPos > bufferSize) return false;
        
        position = newPos;
        return true;
    }

    virtual bool close() override {
        if (buffer) {
            free(buffer);
            buffer = nullptr;
        }
        bufferSize = 0;
        position = 0;
        initialized = false;
        firstPacket = true;
        return true;
    }

    virtual bool isOpen() override { return initialized; }
    virtual uint32_t getSize() override { return bufferSize; }
    virtual uint32_t getPos() override { return position; }
};