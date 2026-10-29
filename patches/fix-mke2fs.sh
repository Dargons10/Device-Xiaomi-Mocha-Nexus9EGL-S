#!/bin/bash
#
# fix_e2fsprogs_lineage.sh
#
# Reaplica los fixes de compatibilidad entre e2fsprogs moderno (Debian 13)
# y las herramientas prebuilt/scripts de AOSP/LineageOS 18.1 (basadas en
# e2fsprogs 1.45.4). Ejecutar desde la RAIZ del árbol de fuentes, DESPUÉS
# de haber lanzado al menos una vez `envsetup.sh + lunch` (para que exista
# out/soong/host/linux-x86/bin/mke2fs).
#
# Uso:
#   cd /ruta/a/lineage-18.1
#   bash fix_e2fsprogs_lineage.sh
#
# Vuelve a ejecutarlo cada vez que:
#   - hagas `repo sync` / `make clean` / `installclean`
#   - abras una terminal nueva (para el fix de PATH, exporta en esa shell)
#   - veas de nuevo "Invalid filesystem option set", "Unknown code ____ 234"
#     o "Invalid sparse file format at header magic"

set -e

SRC_ROOT="$(pwd)"
MKUSERIMG_PY="./system/extras/ext4_utils/mkuserimg_mke2fs.py"
SOONG_MKE2FS="out/soong/host/linux-x86/bin/mke2fs"
HOST_MKE2FS="out/host/linux-x86/bin/mke2fs"
MKE2FS_CONF="/etc/mke2fs.conf"

echo "=== 1/4: Ajustando PATH de esta sesión (quita /sbin y /usr/sbin) ==="
export PATH=$(echo "$PATH" | tr ':' '\n' | grep -v -E '^/s?bin$|^/usr/sbin$' | tr '\n' ':' | sed 's/:$//')
echo "PATH actual: $PATH"
echo "NOTA: esto solo aplica a ESTA shell. Si compilas en otra terminal,"
echo "vuelve a ejecutar este script (o al menos el export de arriba) ahí también."
echo

echo "=== 2/4: Verificando /etc/mke2fs.conf ==="
if [ ! -f "$MKE2FS_CONF" ]; then
  echo "ERROR: no se encontró $MKE2FS_CONF"
  exit 1
fi

if grep -qE "orphan_file|metadata_csum_seed|,64bit|metadata_csum," "$MKE2FS_CONF"; then
  echo "Se detectaron features incompatibles en $MKE2FS_CONF. Requiere sudo para editarlo."
  sudo cp "$MKE2FS_CONF" "${MKE2FS_CONF}.bak.$(date +%s)"
  sudo sed -i \
    -e 's/,orphan_file//g; s/orphan_file,//g; s/\borphan_file\b//g' \
    -e 's/,metadata_csum_seed//g; s/metadata_csum_seed,//g; s/\bmetadata_csum_seed\b//g' \
    -e 's/,64bit//g; s/64bit,//g; s/\b64bit\b//g' \
    -e 's/,metadata_csum\b//g; s/\bmetadata_csum,//g; s/\bmetadata_csum\b//g' \
    "$MKE2FS_CONF"
  echo "Editado. Backup guardado como ${MKE2FS_CONF}.bak.*"
else
  echo "OK: $MKE2FS_CONF ya no contiene las features incompatibles."
fi
echo "Línea 'features =' actual (ext4):"
grep -A3 "^\s*ext4\s*=" "$MKE2FS_CONF" | grep "features" || true
echo

echo "=== 3/4: Restaurando binario mke2fs correcto (prebuilt AOSP) en out/host ==="
if [ ! -f "$SOONG_MKE2FS" ]; then
  echo "AVISO: no existe $SOONG_MKE2FS todavía (¿ya corriste lunch/build al menos una vez?)."
  echo "Saltando este paso; vuelve a correr el script tras el primer intento de build."
else
  if [ -f "$HOST_MKE2FS" ]; then
    SOONG_SIZE=$(stat -c%s "$SOONG_MKE2FS")
    HOST_SIZE=$(stat -c%s "$HOST_MKE2FS")
    if [ "$SOONG_SIZE" != "$HOST_SIZE" ]; then
      echo "Tamaños distintos (soong=$SOONG_SIZE host=$HOST_SIZE) -> restaurando."
      rm -f "$HOST_MKE2FS"
      cp "$SOONG_MKE2FS" "$HOST_MKE2FS"
      chmod +x "$HOST_MKE2FS"
      echo "Restaurado."
    else
      echo "OK: $HOST_MKE2FS ya coincide con el prebuilt correcto."
    fi
  else
    cp "$SOONG_MKE2FS" "$HOST_MKE2FS"
    chmod +x "$HOST_MKE2FS"
    echo "Copiado por primera vez."
  fi
fi
echo "Verificación:"
file "$HOST_MKE2FS" 2>/dev/null || echo "(no existe todavía)"
echo

echo "=== 4/4: Parcheando mkuserimg_mke2fs.py ==="
if [ ! -f "$MKUSERIMG_PY" ]; then
  echo "ERROR: no se encontró $MKUSERIMG_PY (¿ruta de fuentes incorrecta?)"
  exit 1
fi

python3 - "$MKUSERIMG_PY" <<'PYEOF'
import sys, re

path = sys.argv[1]
with open(path) as f:
    content = f.read()

changed = False

# Fix 1: habilitar android_sparse cuando se pide -s (para que e2fsdroid
# reciba una imagen realmente en formato sparse, ya que se invoca sin -e).
variants_old_1 = [
'''  if args.android_sparse:
    pass
    # mke2fs_extended_opts.append("android_sparse")
  else:
    e2fsdroid_opts.append("-e")''',
'''  if args.android_sparse:
    pass  # android_sparse not supported by system e2fsprogs
  e2fsdroid_opts.append("-e")''',
]
new_1 = '''  if args.android_sparse:
    mke2fs_extended_opts.append("android_sparse")
  else:
    e2fsdroid_opts.append("-e")'''

if new_1 not in content:
    applied = False
    for old in variants_old_1:
        if old in content:
            content = content.replace(old, new_1)
            applied = True
            changed = True
            break
    if not applied:
        print("AVISO: no se encontró ninguna variante conocida del bloque android_sparse. "
              "Revisa manualmente esta sección del archivo.")
else:
    print("OK: fix de android_sparse ya estaba aplicado.")

# Fix 2: la lista de features a negar en journal_size==0 debe limitarse a
# ^has_journal, ya que mke2fs 1.45.4 no reconoce metadata_csum_seed ni
# orphan_file (ni siquiera para desactivarlas).
old_2 = 'mke2fs_opts += ["-O", "^has_journal,^metadata_csum_seed,^orphan_file"]'
new_2 = 'mke2fs_opts += ["-O", "^has_journal"]'

if new_2 in content:
    print("OK: fix de ^has_journal ya estaba aplicado.")
elif old_2 in content:
    content = content.replace(old_2, new_2)
    changed = True
else:
    print("AVISO: no se encontró la línea original de ^has_journal,^metadata_csum_seed,^orphan_file. "
          "Puede que ya esté en otro formato; revisa manualmente si el build vuelve a fallar aquí.")

if changed:
    with open(path, 'w') as f:
        f.write(content)
    print("Archivo actualizado: " + path)
else:
    print("Sin cambios necesarios en " + path)
PYEOF

echo
echo "=== Resumen final ==="
echo "--- android_sparse fix ---"
grep -n -A3 "if args.android_sparse:" "$MKUSERIMG_PY"
echo "--- has_journal fix ---"
grep -n "has_journal" "$MKUSERIMG_PY"
echo
echo "Listo. Si vas a relanzar el build ahora, hazlo EN ESTA MISMA SHELL"
echo "(para conservar el PATH sin /sbin) con: brunch mocha"
echo
echo "Si el error ya generó imágenes corruptas, límpialas antes de relanzar, p.ej.:"
echo "  rm -f out/target/product/mocha/*.img"
echo "  rm -rf out/target/product/mocha/obj/PACKAGING/target_files_intermediates/*"
