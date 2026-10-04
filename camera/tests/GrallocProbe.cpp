#include <hardware/gralloc.h>
#include <system/graphics.h>
#include <cstdio>
#include <initializer_list>
int main() {
    const hw_module_t* hw = nullptr;
    int err = hw_get_module(GRALLOC_HARDWARE_MODULE_ID, &hw);
    if (err) return 1;
    auto* module = reinterpret_cast<const gralloc_module_t*>(hw);
    alloc_device_t* dev = nullptr;
    err = gralloc_open(hw, &dev);
    if (err) return 1;
    for (int format : {HAL_PIXEL_FORMAT_YCbCr_420_888, HAL_PIXEL_FORMAT_YV12}) {
        for (int usage : {0x33, 0x20033}) {
            buffer_handle_t buffer = nullptr;
            int stride = 0;
            err = dev->alloc(dev, 1280, 720, format, usage, &buffer, &stride);
            printf("format=%#x usage=%#x alloc=%d stride=%d\n", format, usage, err, stride);
            if (err) continue;
            android_ycbcr yuv = {};
            int lock = module->lock_ycbcr ? module->lock_ycbcr(module, buffer, 0x30, 0, 0, 1280, 720, &yuv) : -1;
            printf("lock_ycbcr=%d Y=%p Cb=%p Cr=%p ystride=%zu cstride=%zu step=%zu\n",lock,yuv.y,yuv.cb,yuv.cr,yuv.ystride,yuv.cstride,yuv.chroma_step);
            if (!lock) module->unlock(module, buffer);
            dev->free(dev, buffer);
        }
    }
    gralloc_close(dev);
}
