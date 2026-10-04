#ifndef MOCHA_CAMERA_PIPELINE_H
#define MOCHA_CAMERA_PIPELINE_H

#include <cstdint>
#include <memory>



#include <system/graphics.h>

#include "isp/DemosaicNEON.h"
#include "isp/ColorConvNEON.h"

namespace mocha {

enum PipelineState {
    PIPELINE_CLOSED,
    PIPELINE_OPENED,
    PIPELINE_STREAMING
};

struct PipelineConfig {
    uint32_t width;
    uint32_t height;
    uint32_t pixelFormat;
    uint8_t bayerPattern;
    uint8_t offset_x;
    uint8_t offset_y;
    bool flipV;
    bool enableISP;
    uint16_t blackLevel;   // 8-bit domain
    uint16_t whiteLevel;   // 8-bit domain, default 255
    float wbGain[4];
    float ccm[9];
    float gamma;
    bool enableAE;
    bool enableAWB;
    float targetLuma;
    float digitalGain;  // software brightness boost (multiplicative on RGB)
};

struct V4l2Buffer {
    void* start;
    size_t length;
    bool allocated;
};

class CameraPipeline {
public:
    CameraPipeline();
    ~CameraPipeline();

    int open(int cameraId);
    int close();

    int configure(const PipelineConfig& config);
    int startStreaming();
    int stopStreaming();

    int captureFrame(uint8_t* outputBuffer, uint32_t outputFormat);
    // Convert the frame just captured while the caller holds pipelineLock.
    int copyCurrentFrame(uint8_t* outputBuffer, uint32_t outputFormat);
    int captureStill(uint8_t* rgba, uint32_t width, uint32_t height);

    PipelineState getState() const { return mState; }

    int setExposure(int exposure);
    int setGain(int gain);
    int getExposure();
    int getGain();

    /* Sensor timing as measured from V4L2 buffer timestamps.
       Used to report real ns in result metadata (not line guesses). */
    int64_t getExposureNs() const;
    int64_t getFramePeriodNs() const { return mFramePeriodNs; }

    int setFocus(int position);
    int getFocusPosition() const { return mFocusPosition; }
    int getAfState() const { return mAfState; }
    int64_t afAgeMs() const;   // ms since last AF scan attempt (huge if never)
    void startAfScan();
    void cancelAf();

private:
    int initFocuser();
    void deinitFocuser();
    int captureForAf();       // Capture a frame during AF sweep, returns sobel energy
    int sobelEnergy(const uint8_t* rgb, int w, int h) const;
    int sobelEnergyNw(const uint8_t* rgb, int w, int h) const; // 8px-wide NEON-style (C reference)
    int afSharpness(const uint8_t* rgb, int w, int h) const;   // AF metric: linear green gradient, ROI + clip-reject

private:
    int processBayerToYuv(const uint8_t* bayerData, uint8_t* output, uint32_t outputFormat);
    void doAutoExposure(const uint8_t* rgbBuffer);
    void doAutoWhiteBalance(const uint8_t* rgbBuffer);
    /* Apply WB gains then CCM in float, clamp to [0,255], write back in place. */
    void applyWbAndCcm(uint8_t* rgb, int total, float rG, float gG, float bG);

    int mFd;
    int mSensorFd;
    int mCameraId;
    PipelineState mState;
    bool mStreaming;

    std::unique_ptr<DemosaicNEON> mDemosaic;
    std::unique_ptr<ColorConvNEON> mColorConv;

    PipelineConfig mConfig;

    V4l2Buffer mBuffers[4];
    int mBufferCount;
    int mCurrentBuffer;

    uint8_t* mRgbBuffer;
    uint32_t mRgbBufferSize;

    int mCurrentExposure;
    int mCurrentGain;

    /* Anti-banding: exposure is quantized to an integer number of mains
       half-cycles (10 ms @ 50 Hz, 8.333 ms @ 60 Hz) so the rolling shutter
       integrates a whole number of light-intensity periods. This removes the
       banding that shows up stronger in stills than in preview. */
    int mExposureStepLines;    // #lines per mains half-cycle (0 = disabled)
    int64_t mMainHalfNs;       // mains half-period target (10 ms / 8.333 ms)
    int mVtsLines;             // vertical total size of active sensor mode
    int mExpMin;               // V4L2_CID_EXPOSURE control range, raw units
    int mExpMax;
    int mExpUnitsPerLine;      // IMX179: 1 unit/line; OV5693: 16 units/line
    int64_t mLinePeriodNs;     // ns per sensor line (measured from frame timestamps)
    int64_t mFramePeriodNs;    // measured full-frame period (median, frozen)
    int64_t mPrevFrameTsNs;    // previous frame timestamp for delta
    int64_t mTsDeltas[16];
    int mDeltaCount;
    bool mTimingFrozen;
    int mAeHoldFrames;         // AE hold countdown (anti-flicker)
    float mAwbGains[4];
    bool mHasAwbInit;
    uint8_t mGammaLut[256];
    float mLastGamma;

    int mFocusPosition;
    int mAfState;
    int64_t mAfLastScanNs;
};

} // namespace mocha

#endif // MOCHA_CAMERA_PIPELINE_H
