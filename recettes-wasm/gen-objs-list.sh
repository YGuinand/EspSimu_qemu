#!/bin/sh
# =====================================================================
# Genere la liste des objets du link final QEMU-wasm (regle c_LINKER de
# meson), dans l'ordre, chemins relatifs au repertoire out/ de l'arbre
# de build. Cette liste est l'entree du bloc wasm d'EspSimu :
#   espsimu.pro : QEMU_OBJ_FILES = $$cat(<fichier>, lines)
#
# Exclusions (parite avec le link du POC C, commit EsSimu a920bfb) :
#   - qemu-system-xtensa.js.p/system_main.c.o : le main() de QEMU est
#     remplace par le thread replique qemu_default_main du backend
#     EspSimu (esp32backend_wasm.cpp) — il ne doit PAS etre lie.
#
# Usage : cd <arbre-out-qemu-wasm> && recettes-wasm/gen-objs-list.sh [sortie]
# (defaut sortie : qemu-objs.txt dans le repertoire courant)
# =====================================================================
set -e
LISTE="${1:-qemu-objs.txt}"
ninja -t commands qemu-system-xtensa.js \
  | tail -n 1 \
  | tr ' ' '\n' \
  | grep '\.o$' \
  | grep -v 'system_main\.c\.o$' \
  > "$LISTE"
echo "$(wc -l < "$LISTE") objets ecrits dans $LISTE"
