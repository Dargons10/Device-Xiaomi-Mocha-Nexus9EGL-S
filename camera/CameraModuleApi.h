/* Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <hardware/camera.h>

/* Stock NVIDIA module (camera.vendor.tegra.so) or nullptr when it failed to load. */
camera_module_t* get_stock_camera_module();

/* Characteristics this wrapper publishes for camera 'id' (0 = imx179, 1 = ov5693).
 * The returned static_camera_characteristics buffer stays valid for the process
 * lifetime, so a HAL3 device may keep a pointer to it. */
int get_mocha_camera_info(int id, camera_info* info);
