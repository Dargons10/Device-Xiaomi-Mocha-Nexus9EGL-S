/*
 * Copyright (C) 2026 The LineageOS Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define _GNU_SOURCE

#include <dlfcn.h>
#include <stdio.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

#include <android/log.h>

/*
 * ABI shim for the stock Tegra124 camera blobs (camera.vendor.tegra.so,
 * libnvmm_camera_v3.so, libnvrm...): they issue the host1x channel SUBMIT
 * ioctl built against the stock 3.10 layout of struct nvhost_submit_args,
 * which was 120 bytes long:
 *
 *     6 x u32 counters, u32 syncpt_incrs (32 bit pointer), u32 fence (out),
 *     u64 cmdbuf_exts, u32 flags, u32 reserved, u64 pad[2],
 *     7 x u64 (cmdbufs, relocs, reloc_shifts, waitchks, waitbases,
 *              class_ids, fences)                                 -> 120 bytes
 *
 * Our kernel carries the upstream-ish commit ea5c4541fe8
 * ("video: tegra: host: Fix submit ioctl signature"), which reordered the
 * same fields into a 128 byte struct (u32 flags moved before u32 fence,
 * syncpt_incrs became u64, pad[3]) -> _IOWR('H', 26, 128).
 *
 * bus_client.c dispatches on the *whole* ioctl number (switch (cmd)), so the
 * blobs' 120 byte variant falls through to "default: -ENOTTY". Measured on
 * the tablet: 1189 ENOTTY in eight seconds, every single submit.
 *
 * The blobs then retry with NVHOST32_IOCTL_CHANNEL_SUBMIT, whose struct has
 * no 'flags' member at all, so the kernel silently drops
 * NVHOST_SUBMIT_FLAG_SYNC_FENCE_FD. nvhost_ioctl_channel_submit() therefore
 * never calls nvhost_sync_create_fence_fd() and stores a raw syncpoint
 * number - or plain 0 when more than one syncpoint is used - in 'fence'
 * instead of a sync fence fd. The HAL wraps that value in android::Fence:
 * every capture request blocked for its full 2000 ms timeout (measured:
 * "process_capture_request -> 0 in 2005 ms", ~0.5 fps preview, preview
 * starting dark because 3A only iterated twice per second), and its
 * destructor closed fd 0, which tripped fdsan ("attempted to close file
 * descriptor 0, expected to be unowned") in the camera provider.
 *
 * Translating the legacy request to the current one keeps 'flags' alive, so
 * the kernel hands back a real sync fence fd, exactly like on the Android 10
 * tree whose kernel still has the 120 byte layout.
 *
 * Only that single command number is rewritten; every other ioctl is passed
 * through untouched. This library is the first entry of the LD_PRELOAD list
 * of android.hardware.camera.provider@2.4-service (see
 * patches/hardware/interfaces/0005-...-preload-mocha-abi-shims.patch), which
 * puts it in the linker's shared group, the only scope
 * camera.vendor.tegra.so / libnvos.so look into.
 */

namespace {

constexpr unsigned kNvhostMagic = 'H';
constexpr unsigned kSubmitCmd = 26;
constexpr unsigned kLegacySize = 120;

struct SubmitLegacy {
    uint32_t submit_version;
    uint32_t num_syncpt_incrs;
    uint32_t num_cmdbufs;
    uint32_t num_relocs;
    uint32_t num_waitchks;
    uint32_t timeout;
    uint32_t syncpt_incrs;   /* 32 bit pointer to struct nvhost_syncpt_incr[] */
    uint32_t fence;          /* out */
    uint64_t cmdbuf_exts;
    uint32_t flags;
    uint32_t reserved;
    uint64_t pad[2];
    uint64_t cmdbufs;
    uint64_t relocs;
    uint64_t reloc_shifts;
    uint64_t waitchks;
    uint64_t waitbases;
    uint64_t class_ids;
    uint64_t fences;
};

struct SubmitCurrent {
    uint32_t submit_version;
    uint32_t num_syncpt_incrs;
    uint32_t num_cmdbufs;
    uint32_t num_relocs;
    uint32_t num_waitchks;
    uint32_t timeout;
    uint32_t flags;
    uint32_t fence;          /* out */
    uint64_t syncpt_incrs;
    uint64_t cmdbuf_exts;
    uint64_t pad[3];
    uint64_t cmdbufs;
    uint64_t relocs;
    uint64_t reloc_shifts;
    uint64_t waitchks;
    uint64_t waitbases;
    uint64_t class_ids;
    uint64_t fences;
};

static_assert(sizeof(SubmitLegacy) == kLegacySize,
              "the stock 3.10 submit ABI must stay 120 bytes");
static_assert(sizeof(SubmitCurrent) == 128,
              "our kernel's submit ABI is 128 bytes (NVHOST_IOCTL_CHANNEL_MAX_ARG_SIZE)");

/* asm-generic/ioctl.h number layout, spelled out so that this file needs no
 * libc prototype at all (bionic declares ioctl() twice with __overloadable,
 * which would conflict with our definition). */
constexpr unsigned kIocDirRW = 3;   /* read | write */
constexpr unsigned kIocDirShift = 30;
constexpr unsigned kIocSizeShift = 16;
constexpr unsigned kIocTypeShift = 8;

constexpr unsigned legacyRequest(unsigned size) {
    return (kIocDirRW << kIocDirShift) | (size << kIocSizeShift) |
           (kNvhostMagic << kIocTypeShift) | kSubmitCmd;
}

/* _IOWR('H', 26, struct nvhost_submit_args) as our kernel computes it. */
constexpr unsigned kCurrentRequest = legacyRequest(sizeof(SubmitCurrent));
/* What strace measured coming out of libnvrm: 0xc078481a. */
constexpr unsigned kLegacyRequest = legacyRequest(kLegacySize);

static_assert(kLegacyRequest == 0xc078481au,
              "the blobs ask for _IOC(RW,'H',26,120) = 0xc078481a");
static_assert(kCurrentRequest == 0xc080481au,
              "our kernel only knows _IOC(RW,'H',26,128) = 0xc080481a");

typedef int (*IoctlFn)(int, unsigned long, ...);

struct SyncptIncr {   /* struct nvhost_syncpt_incr in the kernel */
    uint32_t syncpt_id;
    uint32_t incrs;
};

IoctlFn gRealIoctl = nullptr;
unsigned gTranslations = 0;
int gFirstFence = -1;

}  // namespace

/* bionic declares "int ioctl(int, unsigned, ...)" (bits/ioctl.h); on a 32 bit
 * build int/unsigned/unsigned long are all 32 bit registers, so the calling
 * convention of the blobs is unaffected. */
extern "C" int ioctl(int fd, unsigned request, ...) {
    va_list args;
    va_start(args, request);
    void* argp = va_arg(args, void*);
    va_end(args);

    if (gRealIoctl == nullptr)
        gRealIoctl = (IoctlFn)dlsym(RTLD_NEXT, "ioctl");
    if (gRealIoctl == nullptr) {
        errno = ENOSYS;
        return -1;
    }

    if (fd >= 3 && argp != nullptr && request == kLegacyRequest) {
        SubmitLegacy* legacy = (SubmitLegacy*)argp;
        SubmitCurrent translated;

        memset(&translated, 0, sizeof(translated));
        translated.submit_version = legacy->submit_version;
        translated.num_syncpt_incrs = legacy->num_syncpt_incrs;
        translated.num_cmdbufs = legacy->num_cmdbufs;
        translated.num_relocs = legacy->num_relocs;
        translated.num_waitchks = legacy->num_waitchks;
        translated.timeout = legacy->timeout;
        /* The whole point of this shim: the 32 bit fallback struct cannot
         * carry these flags and the kernel then drops the sync fence fd. */
        translated.flags = legacy->flags;
        translated.syncpt_incrs = legacy->syncpt_incrs;
        translated.cmdbuf_exts = legacy->cmdbuf_exts;
        translated.cmdbufs = legacy->cmdbufs;
        translated.relocs = legacy->relocs;
        translated.reloc_shifts = legacy->reloc_shifts;
        translated.waitchks = legacy->waitchks;
        translated.waitbases = legacy->waitbases;
        translated.class_ids = legacy->class_ids;
        translated.fences = legacy->fences;

        int ret = gRealIoctl(fd, kCurrentRequest, &translated);

        /* The kernel wrote the fence into our struct; the caller expects it
         * at the very same offset in both layouts, but copy it explicitly. */
        legacy->fence = translated.fence;

        if (gTranslations < 12u) {
            const SyncptIncr* incrs =
                    (const SyncptIncr*)(uintptr_t)legacy->syncpt_incrs;
            char sp[128];
            unsigned n = legacy->num_syncpt_incrs;
            unsigned i;
            int len = 0;

            sp[0] = '\0';
            if (incrs != nullptr && n <= 8u) {
                for (i = 0; i < n; i++)
                    len += snprintf(sp + len, sizeof(sp) - (size_t)len,
                            " %u:+%u", incrs[i].syncpt_id, incrs[i].incrs);
            } else {
                snprintf(sp, sizeof(sp), " ?");
            }

            __android_log_print(ANDROID_LOG_INFO, "mocha-hal30",
                    "submit#%u ver=%u cmdbufs=%u syncpts=[%s] relocs=%u "
                    "waitchks=%u waitbases?=%s timeout=%u flags=0x%x -> %s "
                    "fence=%d (0x%x->0x%x)",
                    gTranslations, legacy->submit_version, legacy->num_cmdbufs,
                    sp, legacy->num_relocs, legacy->num_waitchks,
                    legacy->class_ids ? "yes" : "no", legacy->timeout,
                    legacy->flags, ret == 0 ? "ok" : "FAILED",
                    (int)translated.fence, kLegacyRequest, kCurrentRequest);
        }
        gTranslations++;
        gFirstFence = (int)translated.fence;
        return ret;
    }

    return gRealIoctl(fd, request, argp);
}
