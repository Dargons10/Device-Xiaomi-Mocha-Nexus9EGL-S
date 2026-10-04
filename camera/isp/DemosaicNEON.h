/*
 * DemosaicNEON - Bayer to RGB conversion using NEON
 * Malvar-He-Cutler algorithm
 */

#ifndef MOCHA_DEMOSAIC_NEON_H
#define MOCHA_DEMOSAIC_NEON_H

#include <cstdint>
#include <cstddef>

namespace mocha {

struct DemosaicParams {
    uint16_t width;
    uint16_t height;
    uint32_t rawStride;     // V4L2 bytesperline, including row padding
    uint8_t  bayerPattern;  // 0=RGGB, 1=GRBG, 2=GBRG, 3=BGGR
    uint8_t  offset_x;      // X offset for pattern alignment
    uint8_t  offset_y;      // Y offset for pattern alignment
    uint16_t blackLevel;    // 8-bit domain (raw10>>2)
    uint16_t whiteLevel;    // 8-bit domain, default 255
};

class DemosaicNEON {
public:
    DemosaicNEON();
    ~DemosaicNEON();

    int initialize(const DemosaicParams& params);
    void process(const uint8_t* bayerInput, uint8_t* rgbOutput);

private:
    void processRowPairRg(const uint8_t* cur, const uint8_t* prev, const uint8_t* next,
                          uint8_t* out, int w);
    void processRowPairGb(const uint8_t* cur, const uint8_t* prev, const uint8_t* next,
                          uint8_t* out, int w);

    DemosaicParams mParams;
    bool mInitialized;
    uint8_t* mBayerBuf;
    uint32_t mBayerBufSize;
    uint8_t mNormLut[256];  // black-level subtract + white-level normalize
};

} // namespace mocha

#endif // MOCHA_DEMOSAIC_NEON_H