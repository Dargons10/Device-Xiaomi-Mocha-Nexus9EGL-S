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

/*
 * Compatibility shims for the NVIDIA Tegra camera vendor blob
 * (camera.vendor.tegra.so, built against the Android 4.x camera APIs).
 *
 * The blob references a handful of C++ symbols that no longer exist in
 * Android 12 libraries.  Each stub below reproduces the old ABI exactly:
 * ARM C++ member functions pass "this" in r0 and, for the 8-byte
 * sp<> return of createEventQueue(), an sret pointer in r0 with "this"
 * moved to r1 (see the matching signatures in sensor.c).
 */

#define LOG_TAG "MochaCameraShim"

#include <errno.h>
#include <poll.h>
#include <unistd.h>

#include <camera/CameraMetadata.h>

/*
 * android::Fence::wait(unsigned int timeOutMs)
 *
 * The old (pre-Android 5) Fence API took an unsigned millisecond timeout
 * where 0 meant "wait forever".  Android 12 libui only exports
 * Fence::wait(int); the old mangled symbol is gone.  A Fence object is
 * just an int fd member (mFenceFd is the first field, the class has no
 * virtual methods), so polling that fd for POLLIN is exactly what the
 * legacy implementation did on top of a sync_file fd.
 */
extern "C" int _ZN7android5Fence4waitEj(int *self, unsigned int timeOutMs) {
    int fd = *self;
    if (fd < 0)
        return 0; /* invalid fence: treat as already signaled */

    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    int timeout = (timeOutMs == 0u) ? -1 : (int)timeOutMs;
    int ret;
    do {
        ret = ::poll(&pfd, 1, timeout);
    } while (ret < 0 && errno == EINTR);

    if (ret == 0)
        return -ETIMEDOUT;
    if (ret < 0)
        return -errno;
    return 0;
}

/*
 * android::Fence::~Fence()
 *
 * Android 12 declares ~Fence() = default in the header, so the dtor is
 * inlined at every call site and libui exports no FenceD1Ev symbol.  The
 * Fence ctor (from libui, which the blob links against) takes ownership
 * of the fd, so the legacy dtor must close it.
 */
extern "C" void _ZN7android5FenceD1Ev(int *self) {
    int fd = *self;
    if (fd >= 0)
        ::close(fd);
    *self = -1;
}

/*
 * const camera_metadata_t* android::CameraMetadata::getAndLock()
 * void android::CameraMetadata::unlock(const camera_metadata_t*)
 *
 * The old API exposed a non-const getAndLock()/unlock() pair.  Android 12
 * CameraMetadata has the very same locking semantics, just const-ified
 * (mLocked flag + mLock mutex inside the object).  The old non-const
 * mangled symbols therefore have no match in libcamera_client; delegate
 * to the const implementations to keep using the object's own lock.
 */
extern "C" const camera_metadata_t* _ZN7android14CameraMetadata10getAndLockEv(
        android::CameraMetadata *self) {
    return static_cast<const android::CameraMetadata *>(self)->getAndLock();
}

extern "C" void _ZN7android14CameraMetadata6unlockEPK15camera_metadata(
        android::CameraMetadata *self, const camera_metadata_t *metadata) {
    static_cast<const android::CameraMetadata *>(self)->unlock(metadata);
}
