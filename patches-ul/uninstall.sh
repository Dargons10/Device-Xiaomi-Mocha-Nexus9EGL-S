#!/bin/sh

rootdirectory="$PWD"
dirs="system/bt system/libhidl frameworks/base external/dng_sdk packages/apps/Trebuchet packages/apps/Settings lineage-sdk"


for dir in $dirs ; do
	cd $rootdirectory
	cd $dir
	echo "Cleaning $dir patches..."
	git checkout -- . && git clean -df
done

echo "Done!"
cd $rootdirectory
