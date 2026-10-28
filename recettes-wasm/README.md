# Recettes QEMU-wasm (POC C — EsSimu wasm)

Portage du fork (branch `picsimlab-esp32`, lignée lcgamboa/Espressif) sur
**QEMU 10.1** pour un hôte **wasm32/TCI**, consommé par le bloc `wasm {}` de
`EspSimu/espsimu.pro`. **Liaison statique** : QEMU vit dans `espsimu.wasm` —
pas de lib partagée (wasm ne sait pas dlopen).

## Contenu

| Fichier | Rôle |
|---|---|
| `m1-emsdk407.sh` | Recette complète **idempotente** M1 : autotools user-space → meson → déps wasm (zlib, libffi, res-stub, glib 2.75.0, pixman 0.42.2) → QEMU 10.1 out-tree sous **emcc 4.0.7**. Arbre de travail `W=/home/yves/tmp/qemu-wasm-407` (`out/` objets+archives, `target/` déps installées, `builds/` wrapper emcc) ; copie du js/wasm driver vers `/tmp/opencode/qemu-build-407`. |
| `Containerfile` | Image podman `esp-qemu-wasm-base` : tarballs des déps intégrés par `COPY` (plus de réseau dans le build après première fabrication). |
| `gen-objs-list.sh` | Régénère la liste des **668 objets** du link final (règle `c_LINKER` meson, **exclusion `system_main.c.o`** : le `main()` de QEMU est répliqué par le backend EspSimu — voir ci-dessous). Validée identique à la liste canonique du POC C. |
| `driver407.mjs` | Harnais de mesure module-seul (POC B/M1, Node headless) : boot, UART anneau, teardown, métriques JSON. |

## Prérequis

- **emsdk 4.0.7 exact** sur PATH (`source emsdk_env.sh`) — imposé par le kit
  Qt (`emcc_ver.prf` fatal au-delà) : Qt 6.11.2 wasm_multithread.
- meson **1.5.0** (pip user), ninja, perl.
- m4/autoconf/automake/libtool **user-space** (le m4 1.4.19 du système ne
  compile plus sous gcc 16 — voir l'étude, §annexe M1).
- Déps wasm pré-buildées via l'image podman (le script les installe dans
  `$W/target`) ; PKG_CONFIG_LIBDIR réglé pour le cross glib
  (`-Dlibelf=disabled` — voir l'étude).
- Source : un checkout de cette branche (`poc-b`), ex. worktree
  `/tmp/opencode/qemu-101`.

## Usage (ordre)

```sh
podman build -t esp-qemu-wasm-base <répertoire-avec-Containerfile>   # une fois
sh recettes-wasm/m1-emsdk407.sh                                      # idempotent
cd $W/out && sh recettes-wasm/gen-objs-list.sh ../qemu-objs.txt
# côté EspSimu (kit Qt wasm) : qmake + make — voir AGENTS.md
```

## Link côté EsSimu (réplication du link meson)

- les objets réels cités **en brut, dans l'ordre de la règle `c_LINKER`**
  (les registres de types QOM / `.init_array` ne s'activent que si l'objet
  fait partie du lien — une archive ne tire que les symboles référencés) ;
- puis `--start-group libqemuutil.a libfdt.a -lglib-2.0 -lffi -lpcre2-8 -lz
  --end-group` ;
- `-sASYNCIFY=1 -sASYNCIFY_IMPORTS=ffi_call_js` (coroutines wasm
  `emscripten_fiber_swap`), `-sFORCE_FILESYSTEM`, `-sALLOW_TABLE_GROWTH`,
  knobs mkspec `QT_WASM_FIXED_MEMORY` / `QT_WASM_INITIAL_MEMORY=2560MB` /
  `QT_WASM_PTHREAD_POOL_SIZE=8`.

## Traçabilité

La référence de source, les versions d'outils et le sha256 du dernier
`espsimu.wasm` validé sont tenus dans `EspSimu/3rdparty/qemu/VERSION`
(section « Revision QEMU-wasm ») — même discipline que les libs desktop.
