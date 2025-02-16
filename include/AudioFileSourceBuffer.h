#ifndef AUDIOFILESOURCEBUFFER_H
#define AUDIOFILESOURCEBUFFER_H

#include <AudioFileSource.h>

extern uint8_t* opusDataBuffer;
extern size_t opusDataLen;

class AudioFileSourceBuffer : public AudioFileSource {
public:
    AudioFileSourceBuffer() : currentPosition(0) {}

    virtual size_t read(void *data, size_t size) override {
        if (!opusDataBuffer || currentPosition >= opusDataLen) {
            return 0;
        }

        size_t bytesToRead = std::min(size, opusDataLen - currentPosition);
        if (bytesToRead > 0) {
            memcpy(data, opusDataBuffer + currentPosition, bytesToRead);
            
            // Debug first packet read
            if (currentPosition == 0) {
                Serial.print("First OPUS packet: ");
                for (size_t i = 0; i < min(bytesToRead, (size_t)16); i++) {
                    Serial.printf("%02X ", ((uint8_t*)data)[i]);
                }
                Serial.println();
            }
            
            currentPosition += bytesToRead;
        }
        
        return bytesToRead;
    }

    virtual bool isOpen() override {
        return opusDataBuffer != nullptr;
    }

    virtual bool close() override {
        currentPosition = 0;
        return true;
    }

    virtual bool seek(int32_t pos, int dir) override {
        int32_t newPos = currentPosition;
        
        if (dir == SEEK_SET) {
            newPos = pos;
        } else if (dir == SEEK_CUR) {
            newPos += pos;
        } else if (dir == SEEK_END) {
            newPos = opusDataLen + pos;
        }

        if (newPos < 0 || newPos > (int32_t)opusDataLen) {
            Serial.printf("Invalid seek to %d (dir=%d)\n", newPos, dir);
            return false;
        }

        currentPosition = newPos;
        Serial.printf("Seek to %d (dir=%d)\n", currentPosition, dir);
        return true;
    }

    size_t tell() {
        return currentPosition;
    }

    virtual size_t getSize() override {
        return opusDataLen;
    }

private:
    size_t currentPosition;
    
    void dumpBuffer() {
        if (opusDataBuffer && opusDataLen > 0) {
            Serial.println("Buffer contents:");
            for (size_t i = 0; i < min(opusDataLen, (size_t)32); i++) {
                Serial.printf("%02X ", opusDataBuffer[i]);
            }
            Serial.println();
        }
    }
};

#endif // AUDIOFILESOURCEBUFFER_H