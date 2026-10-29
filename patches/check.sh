#!/bin/sh
# Puerta de entrada del registro: comprueba que el arbol es reproducible con el registro
# ANTES de compilar. No modifica nada.
#
# Comprueba tres cosas:
#   1) verify.sh: cada parche esta APPLIED o PENDING (ninguno BROKEN) y ninguno solapa.
#   2) Suciedad de plataforma NO registrada: ficheros modificados en los repos del registro
#      que NO aparecen en ningun parche -> el build no seria reproducible desde git limpio.
#      (Los repos device/kernel/vendor NO se comprueban aqui: esos van commiteados.)
#   3) Que todos los repos del registro existen en el arbol.
#
# Codigo de salida != 0 si algo falla. Funciona desde cualquier directorio.

. "$(cd "$(dirname "$0")" && pwd)/registry_lib.sh"

echo "== verify de parches =="
verify_out=$("$patches_dir/verify.sh")
verify_rc=$?
echo "$verify_out"
echo

unreg=0
for dir in $(list_repos); do
    repo_exists "$dir" || continue
    cd "$tree_root/$dir" || continue
    git status --porcelain 2>/dev/null | awk '{print $2}' | while read -r f; do
        [ -z "$f" ] && continue
        # ? sin parche que la toque?
        touched=0
        for p in $(patches_of "$dir"); do
            if files_of_patch "$p" | grep -qxF "$f"; then touched=1; break; fi
        done
        [ "$touched" -eq 0 ] && echo "UNREGISTERED  $dir/$f"
    done | sort -u
done

# el contador anterior esta en un subshell (pipe), asi que se vuelve a contar aqui
unreg_lines=$(for dir in $(list_repos); do
    repo_exists "$dir" || continue
    cd "$tree_root/$dir" || continue
    git status --porcelain 2>/dev/null | awk '{print $2}' | while read -r f; do
        [ -z "$f" ] && continue
        touched=0
        for p in $(patches_of "$dir"); do
            if files_of_patch "$p" | grep -qxF "$f"; then touched=1; break; fi
        done
        [ "$touched" -eq 0 ] && echo "$dir/$f"
    done
done | sort -u | grep -c . )

echo
if [ "$verify_rc" -ne 0 ]; then
    echo "FALLO: verify.sh ha devuelto error (parches BROKEN o solapados)"
    exit 1
fi
if [ "$unreg_lines" -ne 0 ]; then
    echo "FALLO: $unreg_lines ficheros de plataforma modificados sin parche en el registro"
    echo "       (o el parche correspondente ya no toca ese fichero -> regenerarlo)"
    exit 1
fi
echo "OK: registro coherente con el arbol ($(list_repos | wc -l) repos, $(for d in $(list_repos); do patches_of "$d"; done | wc -l) parches)"
exit 0
