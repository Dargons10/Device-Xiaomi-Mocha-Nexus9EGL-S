#!/bin/sh

echo $1
rootdirectory="$PWD"
# ---------------------------------

dirs="system/bt system/libhidl frameworks/base external/dng_sdk packages/apps/Trebuchet packages/apps/Settings lineage-sdk"

# red + nocolor
RED='\033[0;31m'
NC='\033[0m'

for dir in $dirs ; do
	cd $rootdirectory
	cd $dir
    echo -e "\n${RED}Applying ${NC}$dir ${RED}patches...${NC}\n"
	for p in $rootdirectory/device/xiaomi/mocha/patches-ul/$dir/*.patch ; do
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
