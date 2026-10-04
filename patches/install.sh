#!/bin/sh

echo $1
rootdirectory="$PWD"
# ---------------------------------

dirs="bionic/libm frameworks/native frameworks/av frameworks/base hardware/interfaces hardware/lineage/interfaces system/extras system/core packages/apps/Camera2 packages/apps/Gallery2 hardware/nvidia/hwcomposer hardware/broadcom/wlan hardware/broadcom/fm hardware/broadcom/libbt system/memory/lmkd system/netd system/connectivity/wificond system/sepolicy system/bt vendor/lineage"

# red + nocolor
RED='\033[0;31m'
NC='\033[0m'

for dir in $dirs ; do
	cd $rootdirectory
	cd $dir
    echo -e "\n${RED}Applying ${NC}$dir ${RED}patches...${NC}\n"
	for p in $rootdirectory/device/xiaomi/mocha/patches/$dir/*.patch ; do
		[ -e "$p" ] || continue
		if git apply --check "$p" 2>/dev/null ; then
			git apply -v "$p"
		elif git apply --check -R "$p" 2>/dev/null ; then
			echo "already applied: $(basename $p)"
		else
			echo -e "${RED}FAILED: ${NC}$p"
		fi
	done
done

# -----------------------------------
echo -e "Done !\n"
cd $rootdirectory
