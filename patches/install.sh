#!/bin/sh
# Aplica el registro de parches de plataforma sobre el arbol (orden numerico, porque hay
# parches encadenados que dependen del orden). Idempotente: un parche ya aplicado se
# reconoce con 'git apply --check -R' y se salta. Los conjuntos '*.disabled' se ignoran.
#
# Uso:  sh device/xiaomi/mocha/patches/install.sh      (desde cualquier directorio)
# Codigo de salida != 0 si algun parche no aplica ni directo ni inverso.

. "$(cd "$(dirname "$0")" && pwd)/registry_lib.sh"

fail=0
applied=0
skipped=0

for dir in $(list_repos); do
    if ! repo_exists "$dir"; then
        echo "AVISO: el repo '$dir' no existe en el arbol; se omite"
        continue
    fi
    echo
    echo "== $dir =="
    for p in $(patches_of "$dir"); do
        state=$(patch_state "$dir" "$p")
        case "$state" in
            APPLIED)
                echo "  ya aplicado   $(basename "$p")"
                skipped=$((skipped+1))
                ;;
            PENDING)
                if ( cd "$tree_root/$dir" && git apply "$p" ); then
                    echo "  aplicado      $(basename "$p")"
                    applied=$((applied+1))
                else
                    echo "  FALLO         $(basename "$p")"
                    fail=$((fail+1))
                fi
                ;;
            BROKEN)
                echo "  BROKEN        $(basename "$p")  (ni directo ni inverso)"
                fail=$((fail+1))
                ;;
        esac
    done
done

echo
echo "resumen: applied=$applied ya_aplicados=$skipped fallos=$fail"
[ "$fail" -ne 0 ] && exit 1
exit 0
