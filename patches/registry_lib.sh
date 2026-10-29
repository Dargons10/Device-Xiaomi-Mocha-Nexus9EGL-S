#!/bin/sh
# Helpers compartidos por check.sh / install.sh / uninstall.sh / verify.sh.
#
# Dos errores historicos de estos scripts, corregidos aqui:
#  1) Estaban anclados a "$PWD": si se ejecutaban desde dentro de patches/ (o desde
#     cualquier otro sitio) construian rutas duplicadas tipo
#     .../patches/device/xiaomi/mocha/patches/frameworks/av/*.patch -> no encontraban
#     NINGUN parche y aun asi imprimian "Done !". Ahora todo se ancla a la posicion del
#     propio script, asi que funcionan desde cualquier directorio.
#  2) Cada script llevaba su PROPIA lista hardcodeada de repos, y ninguna coincidia con el
#     registro real (check.sh declaraba 7 repos; install.sh 25; verify.sh 19). Ahora la
#     lista se DERIVA del directorio: cualquier subdirectorio de patches/ con *.patch entra,
#     asi que anadir un parche nuevo no requiere tocar ningun script.
#
# Exclusiones:
#   - "*.disabled"  : los conjuntos sustituidos (frameworks/av.disabled,
#                     frameworks/native.disabled) NO deben aplicarse; se listan aparte.
#   - pending/      : notas de porteo pendientes, no contiene parches aplicables.
#   - README.md y los propios scripts.

patches_dir="$(cd "$(dirname "$0")" && pwd)"
# OJO: patches esta en <arbol>/device/xiaomi/mocha/patches -> son 4 niveles hasta el arbol
# (patches -> mocha -> xiaomi -> device -> arbol). Con 3 daria <arbol>/device y todos los
# repos del registro aparecerian como MISSING-REPO.
tree_root="$(cd "$patches_dir/../../../.." && pwd)"

# Repos (rutas relativas al arbol) que tienen parches en el registro.
list_repos() {
    ( cd "$patches_dir" && find . -mindepth 2 -name '*.patch' -printf '%h\n' 2>/dev/null ) \
        | sed 's|^\./||' \
        | grep -vE '(^|/)[^/]*\.disabled(/|$)' \
        | sort -u
}

# Lo mismo pero SOLO los conjuntos desactivados, para informativos.
list_disabled() {
    ( cd "$patches_dir" && find . -mindepth 2 -name '*.patch' -printf '%h\n' 2>/dev/null ) \
        | sed 's|^\./||' | grep -E '(^|/)[^/]*\.disabled(/|$)' | sort -u
}

# Parches de un repo, en orden numerico (el orden importa: hay dependencias encadenadas).
patches_of() {
    ls -1 "$patches_dir/$1"/*.patch 2>/dev/null | sort
}

# Ficheros que toca un parche (para detectar solapamientos entre parches de un mismo repo).
files_of_patch() {
    grep -e '^diff --git ' "$1" 2>/dev/null | sed -e 's|^diff --git a/||' -e 's| b/.*$||'
}

# Estado de un parche respecto al arbol actual: APPLIED | PENDING | BROKEN
patch_state() {
    _dir="$1"; _p="$2"
    if ( cd "$tree_root/$_dir" && git apply --check -R "$_p" ) 2>/dev/null; then
        echo APPLIED; return
    fi
    if ( cd "$tree_root/$_dir" && git apply --check "$_p" ) 2>/dev/null; then
        echo PENDING; return
    fi
    echo BROKEN
}

# Un repo del registro puede no existir en el arbol (p.ej. si se borr6 de la manifest).
repo_exists() {
    [ -d "$tree_root/$1/.git" ] || [ -d "$tree_root/$1" ]
}
