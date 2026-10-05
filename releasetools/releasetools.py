#
# Copyright (C) 2016 The CyanogenMod Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

# device/xiaomi/mocha/releasetools/releasetools.py
# Copyright 2024 - Xiaomi Mocha

import common
import re

def FullOTA_InstallEnd(info):
    info.script.AppendExtra('''
# Flash TOS Partition
package_extract_file("install/firmware-update/tos.img", "/dev/block/platform/sdhci-tegra.3/by-name/TOS");
# Flash MSC (misc) Partition
package_extract_file("install/firmware-update/misc.img", "/dev/block/platform/sdhci-tegra.3/by-name/MSC");
# Flash USP (blob) Partition
package_extract_file("install/firmware-update/blob", "/dev/block/platform/sdhci-tegra.3/by-name/USP");
''')

def IncrementalOTA_InstallEnd(info):
    info.script.AppendExtra('''
# Flash TOS Partition
package_extract_file("install/firmware-update/tos.img", "/dev/block/platform/sdhci-tegra.3/by-name/TOS");
# Flash MSC (misc) Partition
package_extract_file("install/firmware-update/misc.img", "/dev/block/platform/sdhci-tegra.3/by-name/MSC");
# Flash USP (blob) Partition
package_extract_file("install/firmware-update/blob", "/dev/block/platform/sdhci-tegra.3/by-name/USP");
''')
