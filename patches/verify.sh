#!/bin/sh
# Comprobacion REAL del registro: simula la cadena de parches sin tocar el arbol.
#
# Para cada repo del registro:
#   * Repo con suciedad sin commitear  -> modo DIRECTO: se siembra un directorio temporal
#     con la version HEAD de cada fichero que toque cualquier parche (o sea, el arbol de
#     fabrica) y se aplican los parches en ORDEN NUMERICO con 'git apply -p1'. Despues se
#     compara fichero a fichero contra el arbol de trabajo.
#       - BROKEN   : el parche no aplica en su punto de la cadena (orden roto o parche
#                    desactualizado).
#       - RESIDUAL : tras aplicar toda la cadena el fichero NO coincide con lo que hay en el
#                    arbol -> hay cambios del arbol que el registro no reproduce.
#       - UNREG    : fichero modificado en el repo que no aparece en ningun parche.
#   * Repo sin suciedad (los cambios van commiteados: frameworks/native, kernel/xiaomi/mocha)
#     -> modo INVERSO: se siembra desde HEAD (que ya contiene los cambios) y se revierten los
#     parches en ORDEN INVERSO. Si la cadena inversa completa funciona, el conjunto es
#     coherente. Si ademas existe el fichero BASE con un commit, se compara el resultado con
#     la version de ese commit: prueba de que el registro reproduce el arbol desde ese estado.
#
# No modifica NADA del arbol. Codigo de salida != 0 si hay BROKEN, RESIDUAL o UNREG.

. "$(cd "$(dirname "$0")" && pwd)/registry_lib.sh"

BROKEN=0; RESIDUAL=0; UNREG=0; OK_REPOS=0; CHAIN_OK=0
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT

for repo in $(list_repos); do
    if ! repo_exists "$repo"; then echo "MISSING-REPO  $repo"; continue; fi

    rtmp="$TMP/$(echo "$repo" | tr '/' '_')"
    mkdir -p "$rtmp"

    # 1) ficheros que declara cualquier parche del repo
    files="$rtmp/.files"
    : > "$files"
    for p in $(patches_of "$repo"); do files_of_patch "$p" >> "$files"; done
    sort -u "$files" -o "$files"
    if [ ! -s "$files" ]; then
        echo "SIN-FICHEROS  $repo  (los parches no declaran ningun fichero -> revisarlos)"
        BROKEN=$((BROKEN+1)); continue
    fi

    dirty=$(git -C "$tree_root/$repo" status --porcelain 2>/dev/null | awk '{print $2}' | sort -u)

    # Si el repo declara un BASE, el estado de partida es ese commit (aunque parte de
    # nuestros cambios esten commiteados y parte no: el parche cumulativo cubre ambos).
    if [ -f "$patches_dir/$repo/BASE" ]; then
        seedref=$(cat "$patches_dir/$repo/BASE")
        mode=cumulativo
    elif [ -n "$dirty" ]; then
        seedref=HEAD; mode=directo
    else
        seedref=HEAD; mode=inverso
    fi

    # 2) sembrar el estado de partida
    seeded=0
    while read -r f; do
        [ -z "$f" ] && continue
        mkdir -p "$rtmp/$(dirname "$f")"
        if git -C "$tree_root/$repo" show "$seedref:$f" > "$rtmp/$f" 2>/dev/null; then
            seeded=$((seeded+1))
        else
            rm -f "$rtmp/$f"        # en el arbol de partida ese fichero no existe
        fi
    done < "$files"

    # 3) cadena (ojo: directo/cumulativo aplica hacia delante; inverso revierte al reves)
    chain=ok
    if [ "$mode" != inverso ]; then
        for p in $(patches_of "$repo"); do
            ( cd "$rtmp" && git apply -p1 "$p" ) 2>"$rtmp/.err" || {
                chain=broken; BROKEN=$((BROKEN+1))
                echo "BROKEN        $repo/$(basename "$p")  [directo]"
                sed 's/^/              /' "$rtmp/.err" | head -4
                break
            }
        done
    else
        for p in $(patches_of "$repo" | sed '1!G;h;$!d'); do
            ( cd "$rtmp" && git apply -R -p1 "$p" ) 2>"$rtmp/.err" || {
                chain=broken; BROKEN=$((BROKEN+1))
                echo "BROKEN        $repo/$(basename "$p")  [inverso]"
                sed 's/^/              /' "$rtmp/.err" | head -4
                break
            }
        done
    fi
    [ "$chain" = ok ] && CHAIN_OK=$((CHAIN_OK+1))

    # 4) el resultado de la cadena debe coincidir con lo que se espera
    if [ "$chain" = ok ]; then
        if [ "$mode" = inverso ] && [ ! -f "$patches_dir/$repo/BASE" ]; then
            echo "COMMITED      $repo  (cambios commiteados; cadena inversa OK; anadir $repo/BASE para prueba fuerte)"
        else
            # modo directo o cumulativo: el resultado debe ser el arbol de trabajo actual
            same=1
            while read -r f; do
                [ -z "$f" ] && continue
                if ! diff -q "$rtmp/$f" "$tree_root/$repo/$f" >/dev/null 2>&1; then
                    echo "RESIDUAL      $repo/$f  (la cadena no reproduce el arbol)"; same=0
                    RESIDUAL=$((RESIDUAL+1))
                fi
            done < "$files"
            [ "$same" = 1 ] && echo "REPRODUCIBLE  $repo  ($(patches_of "$repo" | wc -l) parche(s) desde ${seedref} reproducen el arbol)"
            OK_REPOS=$((OK_REPOS+1))
        fi
        [ "$mode" = inverso ] && [ ! -f "$patches_dir/$repo/BASE" ] || true
    fi

    # 5) suciedad que ningun parche declara
    for d in $dirty; do
        grep -qxF "$d" "$files" || { echo "UNREG         $repo/$d  (modificado sin parche)"; UNREG=$((UNREG+1)); }
    done
done

for dir in $(list_disabled); do
    echo "DISABLED      $dir  ($(patches_of "$dir" | wc -l) parches, no se aplican: sustituidos)"
done

echo
echo "cadenas OK=$CHAIN_OK  repos procesados=$OK_REPOS  BROKEN=$BROKEN  RESIDUAL=$RESIDUAL  UNREG=$UNREG"
if [ "$BROKEN" -ne 0 ] || [ "$RESIDUAL" -ne 0 ] || [ "$UNREG" -ne 0 ]; then
    echo "FALLO: el registro no reproduce el arbol"
    exit 1
fi
echo "OK: el registro reproduce el arbol repo a repo"
exit 0
