#!/bin/sh
# Devuelve el arbol al estado limpio de fabrica, revertiendo el registro.
#
#   sh device/xiaomi/mocha/patches/uninstall.sh          reverte parche a parche (orden
#                                                        inverso al de aplicacion) y luego
#                                                        informa de lo que queda sucio.
#   sh device/xiaomi/mocha/patches/uninstall.sh --hard   lo que hacia el script antiguo:
#                                                        git checkout -- . && git clean -df
#                                                        en cada repo del registro. OJO: eso
#                                                        borra TAMBIEN cambios que no esten
#                                                        en el registro; usar solo si se sabe.
#
# El modo por defecto es quirurgico a proposito: si algo no revierte, se reporta en vez de
# pisarse (que era lo que hacia invisible la suciedad no registrada).

. "$(cd "$(dirname "$0")" && pwd)/registry_lib.sh"

mode="revert"
[ "$1" = "--hard" ] && mode="hard"

fail=0
for dir in $(list_repos | sed '1!G;h;$!d'); do   # invertir el orden (revertir al reves)
    repo_exists "$dir" || continue
    if [ "$mode" = "hard" ]; then
        ( cd "$tree_root/$dir" && git checkout -- . && git clean -df ) && echo "== $dir: limpiado (hard)"
        continue
    fi
    for p in $(patches_of "$dir" | sed '1!G;h;$!d'); do
        if ( cd "$tree_root/$dir" && git apply --check -R "$p" ) 2>/dev/null; then
            git -C "$tree_root/$dir" apply -R "$p" && echo "  revertido     $dir/$(basename "$p")"
        elif ( cd "$tree_root/$dir" && git apply --check "$p" ) 2>/dev/null; then
            echo "  no aplicado   $dir/$(basename "$p")  (nada que revertir)"
        else
            echo "  NO REVIORTE   $dir/$(basename "$p")  (queda sucio: revisar)"
            fail=$((fail+1))
        fi
    done
done

if [ "$mode" = "revert" ]; then
    echo
    echo "== resto de suciedad tras revertir =="
    for dir in $(list_repos); do
        repo_exists "$dir" || continue
        out=$(git -C "$tree_root/$dir" status --porcelain 2>/dev/null)
        [ -n "$out" ] && { echo "  $dir:"; echo "$out" | sed 's/^/    /'; fail=$((fail+1)); }
    done
fi

echo
[ "$fail" -ne 0 ] && { echo "quedan cambios sin revertir ($fail)"; exit 1; }
echo "arbol de plataforma revertido al estado de fabrica"
exit 0
