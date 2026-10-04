#include "isp/DemosaicNEON.h"
#include <cstdio>
#include <vector>

int main() {
    // Distinct constant channels expose mixing of green with red/blue.
    const int colors[4][4] = {{0,1,1,2},{1,0,2,1},{1,2,0,1},{2,1,1,0}};
    const int channel[] = {200, 100, 40};
    int failures = 0;
    for (int pattern = 0; pattern < 4; ++pattern) {
        for (int width : {16, 30, 1280, 3264}) {
            const int height = 8;
            const int stride = width + 16;
            std::vector<uint16_t> raw(stride * height, 0xFFFF);
            std::vector<uint8_t> rgb(width * height * 3, 0);
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x)
                    raw[y*stride+x] = channel[colors[pattern][(y%2)*2+x%2]] * 4;
            mocha::DemosaicParams params = {};
            params.width = width; params.height = height; params.rawStride = stride * 2;
            params.bayerPattern = pattern; params.whiteLevel = 255;
            mocha::DemosaicNEON demosaic;
            if (demosaic.initialize(params)) return 2;
            demosaic.process(reinterpret_cast<uint8_t*>(raw.data()), rgb.data());
            bool ok = true;
            for (int y = 1; y < height-1; ++y)
                for (int x = 2; x < width-2; ++x)
                    for (int c = 0; c < 3; ++c)
                        if (rgb[(y*width+x)*3+c] != channel[c]) ok = false;
            std::printf("pattern=%d width=%d constant RGB: %s\n", pattern, width, ok ? "PASS" : "FAIL");
            if (!ok) ++failures;
        }
    }
    return failures ? 1 : 0;
}
