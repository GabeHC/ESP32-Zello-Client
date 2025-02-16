#pragma once
#include <Arduino.h>

struct ZelloAudioPacket {
    static const uint8_t FRAME_MARKER[4];  // {0x6C, 0x2A, 0x0D, 0x01}
    static const size_t HEADER_SIZE = 16;  // 12 null bytes + 4 frame marker
};

struct ZelloStreamInfo {
    String type;           
    String codec;         
    int packet_duration;  
    int stream_id;       
    String from;         
    String codec_header; 
};