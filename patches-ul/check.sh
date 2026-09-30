#!/bin/sh

rootdirectory="$PWD"
dirs="system/bt system/libhidl frameworks/base external/dng_sdk packages/apps/Trebuchet"


RED='\033[0;31m'
NC='\033[0m'

for dir in $dirs ; do
	cd $rootdirectory
	cd $dir
	echo -e "${RED}Applying ${NC}$dir ${RED}patches...${NC}\n"
	git apply -v --check $rootdirectory/device/xiaomi/mocha/patches-ul/$dir/*.patch
done

# -----------------------------------
echo -e "Done !\n"
cd $rootdirectory
