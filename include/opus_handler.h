#pragma once
#include <Arduino.h>
#include <CircularBuffer.hpp>
#include <AudioGeneratorOpus.h>
#include <AudioOutputI2S.h>

#define OPUS_BUFFER_SIZE 8192
#define MIN_BUFFER_SIZE 1024

class OpusHandler {
public:
    OpusHandler(AudioOutputI2S* output);
    bool begin(const char* codecHeader);
    void processAudioData(const uint8_t* data, size_t len);
    void stop();
    bool isRunning();

private:
    CircularBuffer<uint8_t, OPUS_BUFFER_SIZE> buffer;
    AudioGeneratorOpus* opus;
    //AudioFileSourceCircular* source;
    AudioOutputI2S* output;
    bool running;
};

#ifndef OPUS_HANDLER_H
#define OPUS_HANDLER_H

#include <Arduino.h>
#include <AudioOutputI2S.h>
#include <AudioGeneratorOpus.h>

// Forward declarations
class AudioFileSource;

// Function prototypes
uint8_t* decodeBase64(const char* input, size_t* outLen);
void DumpOPUSState();
bool initOpusDecoder(uint8_t* header, size_t headerLen);

#endif