#include "DemosaicNEON.h"
#include <cstring>
#include <cstdlib>
#include <arm_neon.h>

namespace mocha {

DemosaicNEON::DemosaicNEON()
    : mInitialized(false), mBayerBuf(nullptr), mBayerBufSize(0) {
    memset(&mParams, 0, sizeof(mParams));
}

DemosaicNEON::~DemosaicNEON() {
    free(mBayerBuf);
}

int DemosaicNEON::initialize(const DemosaicParams& params) {
    if (params.width < 4 || params.height < 4 || params.bayerPattern > 3 ||
        (params.rawStride && params.rawStride < params.width * 2U)) return -1;
    mParams = params;

    /* Build black-level / white-level normalization LUT.
       raw10_to_8bit yields val10>>2 in [0,255] (8-bit domain). Subtract the
       black level and stretch to full range: out = (v-bl) * 255 / (wl-bl). */
    int bl = params.blackLevel;
    int wl = params.whiteLevel ? params.whiteLevel : 255;
    int den = wl - bl;
    if (den < 1) den = 1;
    for (int i = 0; i < 256; i++) {
        int v = i - bl;
        if (v < 0) v = 0;
        v = (v * 255) / den;
        if (v > 255) v = 255;
        mNormLut[i] = (uint8_t)v;
    }

    uint32_t needed = params.width * params.height + 16;
    if (needed > mBayerBufSize) {
        uint8_t* buf = (uint8_t*)realloc(mBayerBuf, needed + 64);
        if (!buf) return -1;
        mBayerBuf = buf;
        mBayerBufSize = needed + 64;
    }
    mInitialized = true;
    return 0;
}

static void raw10_to_8bit(const uint8_t* in, uint8_t* out, int total, const uint8_t* lut) {
    /* The VI (T_R16_I + CSI DT_RAW10) stores each 10-bit sample RIGHT-ALIGNED
       in a little-endian 16-bit word: val10 = v & 0x3FF (0..1023). Convert to
       8-bit with v >> 2, then apply the black/white-level LUT.
       Reading only the low byte (& 0xFF) mod-wraps every val10 > 255,
       scrambling highlights into magenta/cyan speckle (the color bug). */
    for (int i = 0; i < total; i++) {
        uint32_t v = ((uint32_t)in[i*2] | ((uint32_t)in[i*2+1] << 8)) & 0x3FF;
        out[i] = lut[v >> 2];
    }
}

static inline uint8_t clamp(int v) {
    return (uint8_t)((v >> 8) ? (v < 0 ? 0 : 255) : v);
}

static void fill_edges(uint8_t* rgb, int w, int h) {
    int s = w * 3;
    memcpy(rgb, rgb + s, s);
    memcpy(rgb + (h-1)*s, rgb + (h-2)*s, s);
    for (int y = 0; y < h; y++) {
        int off = y * s;
        rgb[off] = rgb[off + 3];
        rgb[off + 1] = rgb[off + 4];
        rgb[off + 2] = rgb[off + 5];
        rgb[off + (w-1)*3] = rgb[off + (w-2)*3];
        rgb[off + (w-1)*3 + 1] = rgb[off + (w-2)*3 + 1];
        rgb[off + (w-1)*3 + 2] = rgb[off + (w-2)*3 + 2];
    }
}

/* Bilinear interpolation. Calculate H/V/diagonal neighbours at the same
 * pixel before selecting channels, including both green Bayer positions. */
static void neon_row(const uint8_t* row, const uint8_t* up, const uint8_t* dn,
                     uint8_t* rgb, int w, int out_base,
                     int ev_type, int od_type) {
    auto scalar = [&](int x) {
        const int l = x > 0 ? x - 1 : 1;
        const int r = x + 1 < w ? x + 1 : w - 2;
        int c = row[x], h = (row[l] + row[r]) / 2;
        int v = (up[x] + dn[x]) / 2;
        int hv = (row[l] + row[r] + up[x] + dn[x]) / 4;
        int d = (up[l] + up[r] + dn[l] + dn[r]) / 4;
        int type = (x & 1) ? od_type : ev_type;
        int off = out_base + x * 3;
        rgb[off] = type == 0 ? c : type == 1 ? h : type == 2 ? v : d;
        rgb[off+1] = (type == 1 || type == 2) ? c : hv;
        rgb[off+2] = type == 3 ? c : type == 1 ? v : type == 2 ? h : d;
    };
    scalar(0); scalar(1);
    const uint8_t parity[8] = {255,0,255,0,255,0,255,0};
    const uint8x8_t evenMask = vld1_u8(parity);
    int x = 2;
    for (; x + 8 < w; x += 8) {
        uint8x8_t c = vld1_u8(row+x);
        uint16x8_t hs = vaddl_u8(vld1_u8(row+x-1), vld1_u8(row+x+1));
        uint16x8_t vs = vaddl_u8(vld1_u8(up+x), vld1_u8(dn+x));
        uint8x8_t h = vshrn_n_u16(hs, 1), v = vshrn_n_u16(vs, 1);
        uint8x8_t hv = vshrn_n_u16(vaddq_u16(hs,vs), 2);
        uint16x8_t ds = vaddq_u16(
            vaddl_u8(vld1_u8(up+x-1),vld1_u8(up+x+1)),
            vaddl_u8(vld1_u8(dn+x-1),vld1_u8(dn+x+1)));
        uint8x8_t d = vshrn_n_u16(ds, 2);
        uint8x8_t red[4] = {c,h,v,d};
        uint8x8_t green[4] = {hv,c,c,hv};
        uint8x8_t blue[4] = {d,v,h,c};
        uint8x8x3_t out;
        out.val[0] = vbsl_u8(evenMask,red[ev_type],red[od_type]);
        out.val[1] = vbsl_u8(evenMask,green[ev_type],green[od_type]);
        out.val[2] = vbsl_u8(evenMask,blue[ev_type],blue[od_type]);
        vst3_u8(rgb+out_base+x*3, out);
    }
    for (; x < w; ++x) scalar(x);
}

void DemosaicNEON::process(const uint8_t* bayerInput, uint8_t* rgbOutput) {
    if (!mInitialized || !bayerInput || !rgbOutput || !mBayerBuf) return;

    const int w = mParams.width;
    const int h = mParams.height;
    const int s = w * 3;
    const int oy = mParams.offset_y & 1;
    const int ox = mParams.offset_x & 1;
    const int pat = mParams.bayerPattern;

    const uint32_t stride = mParams.rawStride ? mParams.rawStride : w * 2;
    for (int y = 0; y < h; ++y)
        raw10_to_8bit(bayerInput + y * stride, mBayerBuf + y * w, w, mNormLut);
    const uint8_t* b8 = mBayerBuf;

    /* pos_color[pat][pos] -> type. pos = (rowParity*2 + colParity):
         0=(even,even) 1=(even,odd) 2=(odd,even) 3=(odd,odd)
       type: 0=R 1=G1(R=H,B=V) 2=G2(R=V,B=H) 3=B
       Standard Bayer order:
         pat0 RGGB: (0,0)=R (0,1)=G1 (1,0)=G2 (1,1)=B
         pat1 GRBG: (0,0)=G1 (0,1)=R (1,0)=B (1,1)=G2
         pat2 GBRG: (0,0)=G2 (0,1)=B (1,0)=R (1,1)=G1
         pat3 BGGR: (0,0)=B (0,1)=G2 (1,0)=G1 (1,1)=R
    */
    static const uint8_t pos_color[4][4] = {
        {0, 1, 2, 3}, /* pat 0 = RGGB */
        {1, 0, 3, 2}, /* pat 1 = GRBG */
        {2, 3, 0, 1}, /* pat 2 = GBRG */
        {3, 2, 1, 0}, /* pat 3 = BGGR */
    };

    for (int y = 1; y < h - 1; y++) {
        int r = (y + oy) & 1;
        int pe = r * 2 + ox;
        int po = r * 2 + 1 - ox;
        int ev_type = pos_color[pat][pe];
        int od_type = pos_color[pat][po];
        int out_base = y * s;

        const uint8_t* row = b8 + y * w;
        const uint8_t* up  = b8 + (y - 1) * w;
        const uint8_t* dn  = b8 + (y + 1) * w;

        neon_row(row, up, dn, rgbOutput, w, out_base, ev_type, od_type);
    }

    fill_edges(rgbOutput, w, h);
}

} // namespace mocha
