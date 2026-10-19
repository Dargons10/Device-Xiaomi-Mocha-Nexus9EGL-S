/* Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <camera/CameraMetadata.h>

namespace mocha {
constexpr uint8_t kPipelineDepth = 8;

bool buildCharacteristics(const camera_metadata_t* stock, android::CameraMetadata* output);
bool normalizeRequest(android::CameraMetadata* request, const camera_metadata_t* characteristics);
bool mergeMetadata(android::CameraMetadata* destination, const camera_metadata_t* source);
bool finishResult(android::CameraMetadata* result, const android::CameraMetadata& partials);
}  // namespace mocha
