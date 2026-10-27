#!/bin/sh
# Estado real de cada parche del chain (tri-state), sin modificar nada.
#   APPLIED : el parche ya esta aplicado (git apply -R --check pasa)
#   PENDING : el parche aplica limpio sobre el arbol actual (falta aplicarlo)
#   BROKEN  : no aplica ni directo ni inverso (parche desactualizado/conflicto)

rootdirectory="$PWD"
set -- \
    bionic/libm \
    frameworks/native \
    frameworks/av \
    frameworks/base \
    hardware/interfaces \
    hardware/lineage/interfaces \
    hardware/broadcom/wlan \
    hardware/broadcom/fm \
    hardware/broadcom/libbt \
    system/core \
    system/extras \
    system/memory/lmkd \
    system/netd \
    system/connectivity/wificond \
    system/sepolicy \
    system/bt \
    packages/apps/Camera2 \
    packages/apps/Gallery2 \
    vendor/lineage

APPLIED=0; PENDING=0; BROKEN=0; MISSING=0

for dir in "$@"; do
    patchdir="$rootdirectory/device/xiaomi/mocha/patches/$dir"
    if [ ! -d "$patchdir" ]; then
        continue
    fi
    if [ ! -d "$rootdirectory/$dir/.git" ]; then
        echo "MISSING-REPO  $dir"
        MISSING=$((MISSING+1))
        continue
    fi
    for p in "$patchdir"/*.patch; do
        [ -e "$p" ] || continue
        name="${dir}/$(basename "$p")"
        ( cd "$rootdirectory/$dir" && git apply --check -R "$p" ) 2>/dev/null && {
            echo "APPLIED       $name"; APPLIED=$((APPLIED+1)); continue; }
        ( cd "$rootdirectory/$dir" && git apply --check "$p" ) 2>/dev/null && {
            echo "PENDING       $name"; PENDING=$((PENDING+1)); continue; }
        echo "BROKEN        $name"; BROKEN=$((BROKEN+1))
    done
done

echo
echo "resumen: APPLIED=$APPLIED PENDING=$PENDING BROKEN=$BROKEN MISSING-REPO=$MISSING"
