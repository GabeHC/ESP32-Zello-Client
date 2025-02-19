#pragma once

#include <AudioFileSource.h>
#include <ogg/ogg.h>

static const size_t OGG_HEADER_SIZE = 27;
static const size_t SEGMENT_TABLE_SIZE = 1;
static const size_t OPUS_HEADER_SIZE = 19;
static const size_t OPUS_TAGS_SIZE   = 16;
static const size_t WS_HEADER_SIZE   = 9;
static const uint8_t WS_TYPE_AUDIO   = 0x82;

struct ZelloOpusConfig {
    uint16_t sampleRate;
    uint8_t framesPerPacket;
    uint8_t frameSizeMs;
};

class AudioFileSourceMemory : public AudioFileSource {
protected:
    const uint8_t* _data;
    size_t _size;
    size_t _pos;
    ZelloOpusConfig _config;

    uint8_t* _oggBuffer;
    size_t _oggSize;

public:
    AudioFileSourceMemory()
        : _data(nullptr),
          _size(0),
          _pos(0),
          _oggBuffer(nullptr),
          _oggSize(0)
    {
        memset(&_config, 0, sizeof(_config));
    }

    ~AudioFileSourceMemory() {
        if (_oggBuffer) {
            delete[] _oggBuffer;
            _oggBuffer = nullptr;
        }
    }

    bool setData(const uint8_t* data, size_t len)
    {
        if (!data || len == 0) return false;

        // Skip WebSocket header if present
        if (len > WS_HEADER_SIZE && data[0] == WS_TYPE_AUDIO) {
            data += WS_HEADER_SIZE;
            len  -= WS_HEADER_SIZE;
        }

        // Page 1 size = header(27) + 1 segment + OpusHead(19) = 27 + 1 + 19 = 47
        size_t firstPageSize  = OGG_HEADER_SIZE + SEGMENT_TABLE_SIZE + OPUS_HEADER_SIZE;
        // Page 2 size = header(27) + 1 segment + OpusTags(16) = 27 + 1 + 16 = 44
        size_t secondPageSize = OGG_HEADER_SIZE + SEGMENT_TABLE_SIZE + OPUS_TAGS_SIZE;

        // For the 3rd page (audio data), we break it into segments of at most 255 bytes each
        uint32_t numSegments = (len + 254) / 255;
        if (numSegments > 255) {
            return false; // Would need multiple pages
        }

        size_t thirdPageSize = OGG_HEADER_SIZE + numSegments + len;
        _oggSize = firstPageSize + secondPageSize + thirdPageSize;

        _oggBuffer = new uint8_t[_oggSize];
        if (!_oggBuffer) {
            return false;
        }

        // ----- Page 1: OpusHead -----
        uint8_t* ptr = _oggBuffer;
        memcpy(ptr, "OggS", 4); // magic
        ptr[4] = 0;             // version
        ptr[5] = 0x02;          // BOS
        memset(ptr + 6, 0, 8);  
        uint32_t serialNo = 0x1234;
        memcpy(ptr + 14, &serialNo, 4);
        uint32_t pageNo = 0;
        memcpy(ptr + 18, &pageNo, 4);
        memset(ptr + 22, 0, 4);
        ptr[26] = 1;                       // one segment
        ptr[27] = OPUS_HEADER_SIZE;        // segment size
        // Write OpusHead
        ptr += OGG_HEADER_SIZE + SEGMENT_TABLE_SIZE;
        memcpy(ptr, "OpusHead", 8);
        ptr[8] = 1;                 // version
        ptr[9] = 1;                 // channel count (mono)
        uint16_t preskip = 0;
        memcpy(ptr + 10, &preskip, 2); // pre-skip
        uint32_t sampleRate = 16000;   
        memcpy(ptr + 12, &sampleRate, 4); 
        uint16_t gain = 0;
        memcpy(ptr + 16, &gain, 2);    // output gain
        ptr[18] = 0;                   // mapping family

        // ----- Page 2: OpusTags -----
        ptr = _oggBuffer + firstPageSize;
        memcpy(ptr, "OggS", 4); 
        ptr[4] = 0;            
        ptr[5] = 0;            // no BOS/EOS
        memset(ptr + 6, 0, 8);
        pageNo = 1;
        memcpy(ptr + 14, &serialNo, 4);
        memcpy(ptr + 18, &pageNo, 4);
        memset(ptr + 22, 0, 4);
        ptr[26] = 1;                       // one segment
        ptr[27] = OPUS_TAGS_SIZE;         // segment size
        ptr += OGG_HEADER_SIZE + SEGMENT_TABLE_SIZE;
        uint8_t opusTags[OPUS_TAGS_SIZE] = {'O','p','u','s','T','a','g','s',0,0,0,0,0,0,0,0};
        memcpy(ptr, opusTags, OPUS_TAGS_SIZE);

        // ----- Page 3: Raw data -----
        ptr = _oggBuffer + firstPageSize + secondPageSize;
        memcpy(ptr, "OggS", 4);
        ptr[4] = 0;       // version
        ptr[5] = 0;       // no BOS/EOS
        memset(ptr + 6, 0, 8);
        pageNo = 2;
        memcpy(ptr + 14, &serialNo, 4);
        memcpy(ptr + 18, &pageNo, 4);
        memset(ptr + 22, 0, 4);
        ptr[26] = (uint8_t)numSegments;  // number of segments
        ptr += OGG_HEADER_SIZE;

        // Fill the segment table entries with up to 255 each
        size_t dataRemaining = len;
        for (uint32_t i = 0; i < numSegments; i++) {
            uint8_t segLen = (dataRemaining > 255) ? 255 : (uint8_t)dataRemaining;
            ptr[i] = segLen;
            dataRemaining -= segLen;
        }

        // Now copy data after the segment table
        ptr += numSegments;
        memcpy(ptr, data, len);

        // Save state
        _pos  = 0;
        _data = data;
        _size = len;
        _config.sampleRate     = 16000;
        _config.framesPerPacket = 6;
        _config.frameSizeMs    = 20;
        return true;
    }

    virtual size_t read(void* outData, size_t len) override {
        if (!_oggBuffer || _pos >= _oggSize) {
            return 0;
        }
        size_t toRead = (len > (_oggSize - _pos)) ? (_oggSize - _pos) : len;
        memcpy(outData, _oggBuffer + _pos, toRead);
        _pos += toRead;
        return toRead;
    }

    virtual uint32_t getSize() override {
        return static_cast<uint32_t>(_oggSize);
    }

    virtual uint32_t getPos() override {
        return static_cast<uint32_t>(_pos);
    }

    const ZelloOpusConfig& getOpusConfig() const {
        return _config;
    }

    void setOpusConfig(const ZelloOpusConfig& config) {
        _config = config;
    }

    uint32_t position() {
        return getPos();
    }
};