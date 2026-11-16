# Puerta de registro (T-02): ningun build COMPLETO de mocha arranca si el registro de
# parches no reproduce el arbol (check.sh = verify.sh + suciedad sin registrar).
#
# Se engancha como prerequisito de los dos targets phony que usa soong_ui:
#   build/make/core/main.mk:15  .PHONY: droid
#   build/make/core/main.mk:20  droid_targets:
# 'm <modulo>' (job_module.sh) NO depende de ellos, asi que el desarrollo modular sigue
# siendo rapido y sin esta puerta.
#
# Saltarselo conscientemente:  MOCHA_SKIP_REGISTRY_CHECK=1 brunch lineage_mocha

MOCHA_REGISTRY_SCRIPT := $(LOCAL_PATH)/patches/check.sh

.PHONY: mocha-registry-check
mocha-registry-check:
	@if [ "$$MOCHA_SKIP_REGISTRY_CHECK" = "1" ]; then \
		echo "mocha: AVISO: comprobacion del registro OMITIDA (MOCHA_SKIP_REGISTRY_CHECK=1)"; \
	else \
		echo "mocha: comprobando que device/xiaomi/mocha/patches reproduce el arbol..."; \
		sh $(MOCHA_REGISTRY_SCRIPT) || { \
			echo "mocha: FALLO el registro de parches. Corrige BROKEN/RESIDUAL/UNREG"; \
			echo "       (sh device/xiaomi/mocha/patches/verify.sh) o exporta MOCHA_SKIP_REGISTRY_CHECK=1"; \
			exit 1; \
		}; \
	fi

droid: mocha-registry-check
droid_targets: mocha-registry-check
