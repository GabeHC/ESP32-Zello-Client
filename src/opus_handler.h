#pragma once

#include <Arduino.h>
#include <AudioOutputI2S.h>
#include <AudioGeneratorOpus.h>
#include "audio_file_source_memory.h"

// OPUS Header Constants
#define OPUS_HEADER_MAGIC "OpusHead"
#define OPUS_HEADER_SIZE 19
#define OPUS_VERSION 1
#define OPUS_SAMPLE_RATE 48000
#define OPUS_CHANNELS 1
#define OPUS_PRESKIP 0
#define OPUS_GAIN 0

class OpusHandler {
private:
    AudioGeneratorOpus* opus;
    AudioOutputI2S* output;
    AudioFileSourceMemory* fileSource;
    bool running;
    uint8_t* buffer;
    size_t bufferSize;
    size_t bufferPos;

    // Add private helper methods
    bool createOpusHeader(uint8_t* header);
    bool formatOpusStream();

public:
    OpusHandler(AudioOutputI2S* out);
    ~OpusHandler();
    
    bool begin();
    bool processAudioData(const uint8_t* data, size_t len);
    bool isRunning();
    void stop();
    bool loop();
    
    // Add helper methods
    size_t read(uint8_t* dest, size_t length);
    bool seek(size_t pos);
    size_t position() { return bufferPos; }
    size_t available() { return bufferSize - bufferPos; }
};