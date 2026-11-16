# Puerta de registro (T-02): ningun build COMPLETO de mocha debe empaquetar nada si el
# registro de parches no reproduce el arbol (check.sh = verify.sh + suciedad sin registrar).
#
# OJO con la forma: la primera version de este fichero hacia `mocha-registry-check` un target
# .PHONY y se lo enganchaba a `droid`.  Medido el 2026-11-16 con `./job_module.sh kernel`:
#     device/xiaomi/mocha/patches/registry_gate.mk:27: error: real file "droid" depends on
#     PHONY target "mocha-registry-check"
#     ckati failed with: exit status 1
# Es decir: **ckati prohíbe que un target real dependa de uno phony**, y en kati `droid` es un
# target real.  Ahora el gate es un fichero stamp real, o sea real -> real.
#
# El stamp se regenera cuando cambia cualquier pieza del registro (parches, BASE, scripts).
# La suciedad de los repos de plataforma NO es una dependencia de fichero, así que para
# comprobarla SIEMPRE existen dos vías, y ambas están en uso:
#   - `job_module.sh` ejecuta check.sh antes de compilar cualquier modulo (aborta con rc=6;
#     `SKIP_PATCH_CHECK=1` para saltarlo conscientemente).
#   - `make mocha-registry-check` (target phony, phony -> real si está permitido) fuerza la
#     comprobación en cualquier momento.
#
# Saltarse la puerta en el build completo: MOCHA_SKIP_REGISTRY_CHECK=1 brunch lineage_mocha

MOCHA_REGISTRY_SCRIPT := $(LOCAL_PATH)/patches/check.sh
MOCHA_REGISTRY_STAMP := $(OUT_DIR)/mocha-registry-check.txt
MOCHA_REGISTRY_INPUTS := $(shell find $(LOCAL_PATH)/patches \( -name '*.patch' -o -name 'BASE' -o -name '*.sh' \) -print 2>/dev/null)

ifneq ($(strip $(OUT_DIR)),)
$(MOCHA_REGISTRY_STAMP): $(MOCHA_REGISTRY_SCRIPT) $(MOCHA_REGISTRY_INPUTS)
	@if [ "$$MOCHA_SKIP_REGISTRY_CHECK" = "1" ]; then \
		echo "mocha: AVISO: comprobacion del registro OMITIDA (MOCHA_SKIP_REGISTRY_CHECK=1)" > $@; \
	else \
		echo "mocha: comprobando que device/xiaomi/mocha/patches reproduce el arbol..."; \
		sh $(MOCHA_REGISTRY_SCRIPT) > $@ 2>&1; rc=$$?; cat $@; \
		nrepos=`sed -n 's/.*repos procesados=\([0-9]*\).*/\1/p' $@ | head -1`; \
		if [ -z "$$nrepos" ] || [ "$$nrepos" -lt 1 ]; then \
			echo "mocha: FALLO: el chequeo no proceso NINGUN repo del registro. Eso NO es un OK:"; \
			echo "       o el registro esta vacio, o la herramienta que busca los parche falto (dentro del"; \
			echo "       build, 'find' es toybox 0.8.4 y no soporta '-printf %h'). Ver registry_lib.sh."; \
			rm -f $@; exit 1; \
		fi; \
		if [ $$rc -ne 0 ]; then \
			echo "mocha: FALLO el registro de parches. Corrige BROKEN/RESIDUAL/UNREG"; \
			echo "       (sh device/xiaomi/mocha/patches/verify.sh) o exporta MOCHA_SKIP_REGISTRY_CHECK=1"; \
			rm -f $@; exit 1; \
		fi; \
	fi

droid: $(MOCHA_REGISTRY_STAMP)
droid_targets: $(MOCHA_REGISTRY_STAMP)

.PHONY: mocha-registry-check
mocha-registry-check: $(MOCHA_REGISTRY_STAMP)
endif
