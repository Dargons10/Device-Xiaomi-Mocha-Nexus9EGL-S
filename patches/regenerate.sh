#!/bin/sh
# regenerate.sh <repo> [--force]
#
# Vuelve a generar el parche CUMULATIVO de un repo para que el registro vuelva a reproducir
# el arbol de trabajo.  Es la herramienta que faltaba cuando se edita un fichero de plataforma
# directamente y despues check.sh responde UNREG/RESIDUAL.
#
# Semilla (igual que verify.sh):
#   - el repo tiene BASE  -> diff  BASE..arbol de trabajo   (modo cumulativo)
#   - el repo sin BASE    -> diff  HEAD..arbol de trabajo   (modo directo)
#
# Protecciones:
#   - Se niega a tocar un repo con MAS DE UN parche activo (las cadenas ordenadas de
#     system/core, hardware/interfaces... no se pueden colapsar en un cumulativo).  Para eso
#     hay que regenerar el parche afectado a mano, o pasar --force sabiendo lo que haces.
#   - Guarda el parche anterior como <nombre>.bak-<fecha> antes de reescribirlo.
#   - No commitea nada: despues hay que comprobar con check.sh y commitear el DT.
#
# uso:  sh device/xiaomi/mocha/patches/regenerate.sh packages/apps/Camera2
#       sh device/xiaomi/mocha/patches/regenerate.sh frameworks/av

. "$(cd "$(dirname "$0")" && pwd)/registry_lib.sh"

repo="${1:?falta el repo, p.ej. packages/apps/Camera2}"
force="${2:-}"
[ "$force" = "--force" ] || force=""

repo_exists "$repo" || { echo "no existe $repo en $tree_root"; exit 1; }
ps=$(patches_of "$repo")
n=$(printf '%s\n' $ps | grep -c .)
if [ "$n" -eq 0 ]; then echo "no hay parches en $patches_dir/$repo"; exit 1; fi
if [ "$n" -gt 1 ] && [ -z "$force" ]; then
    echo "ERROR: $repo tiene $n parches activos (cadena ordenada). Regenerar un cumulativo"
    echo "       colapsaria la cadena. Arregla el parche concreto, o usa --force."
    printf '%s\n' $ps | sed 's/^/  /'
    exit 1
fi

seed=$(cat "$patches_dir/$repo/BASE" 2>/dev/null)
if [ -n "$seed" ]; then modo="cumulativo (BASE)"; else seed=$(git -C "$tree_root/$repo" rev-parse HEAD); modo="directo (HEAD)"; fi

dirty=$(git -C "$tree_root/$repo" status --porcelain | wc -l)
ahead=$(git -C "$tree_root/$repo" rev-list --count "$seed..HEAD" 2>/dev/null || echo 0)

echo "repo   : $repo"
echo "seed   : $seed  [$modo]"
echo "commits locales sobre la semilla: $ahead"
echo "ficheros sucios ahora           : $dirty"

primero=$(printf '%s\n' $ps | head -1)
resto=$(printf '%s\n' $ps | tail -n +2)
if [ -n "$resto" ]; then
    echo "AVISO: se colapsan $n parches en uno solo; los descartados se conservan en"
    printf '%s\n' $resto | sed 's/^/  descartaria: /'
    echo "         (se mueven a $(basename "$repo").disabled/)"
fi

ts=$(date +%Y%m%d-%H%M%S)
old=$(basename "$primero")
bak="$patches_dir/$repo/.discard-$old.$ts"
tmp="$patches_dir/$repo/.new-$old"

git -C "$tree_root/$repo" diff "$seed" -- . > "$tmp"
if [ ! -s "$tmp" ]; then echo "ERROR: el diff sale vacio; el arbol ya esta commiteado sobre la semilla?"; exit 1; fi
if ! ( cd "$tree_root/$repo" && git stash list >/dev/null 2>&1 && git apply --check "$tmp" 2>/dev/null ); then
    echo "AVISO: el cumulativo no aplica sobre el arbol tal cual (normal si hay cambios commiteados"
    echo "       sobre la semilla: verify.sh lo comprobara sembrando desde la semilla)."
fi
mv "$primero" "$bak"
mv "$tmp" "$primero"
echo "parche regenerado : $repo/$old  ($(grep -c '^diff --git ' "$primero") ficheros)"
echo "anterior guardado : $repo/$(basename "$bak")"
if [ -n "$resto" ]; then
    mkdir -p "$patches_dir/$repo.disabled"
    for p in $resto; do mv "$p" "$patches_dir/$repo.disabled/$(basename "$p")"; echo "  movido a $(basename "$repo").disabled/"; done
fi
echo
echo "siguiente paso: sh device/xiaomi/mocha/patches/check.sh   y luego commitear device/xiaomi/mocha"
