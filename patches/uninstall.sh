#!/bin/sh

rootdirectory="$PWD"
dirs=" bionic/libm frameworks/native frameworks/av frameworks/base hardware/interfaces system/extras system/core packages/apps/Camera2 packages/apps/Gallery2 hardware/nvidia/hwcomposer hardware/broadcom/wlan hardware/broadcom/fm hardware/broadcom/libbt system/memory/lmkd system/netd system/connectivity/wificond system/sepolicy system/bt vendor/lineage"


for dir in $dirs ; do
	cd $rootdirectory
	cd $dir
	echo "Cleaning $dir patches..."
	git checkout -- . && git clean -df
done

echo "Done!"
cd $rootdirectory
