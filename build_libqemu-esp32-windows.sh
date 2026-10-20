#!/bin/sh

# Cross-compile libqemu-xtensa.dll et libqemu-riscv32.dll pour Windows (MinGW).
#
# Methode (celle qui produit des PE32+ valides) :
#   1. configure croise MinGW (slirp + gcrypt actives ; tenir a jour le gcrypt.pc
#      fait main dans /tmp/mingw-pc/), build out-of-tree dans qemu-build-win/
#   2. ninja compile tous les objets (les links d'exes echouent, on s'en fiche)
#   3. on recupere la commande de link EXACTE de chaque exe via
#      `ninja -t commands qemu-system-<arch>.exe`, on la transforme en link de
#      DLL : -shared, suppression de system_main.c.obj, ajout des aliases
#      __imp_slirp_*  (le code QEMU importe slirp en dllimport alors que slirp
#      est statique), -lgcrypt -lgpg-error explicites, --export-all-symbols.
#      C'est l'equivalent Windows du sed sur le .rsp du script Linux.
#
# Pre-requis : x86_64-w64-mingw32-{gcc,ar,nm,pkg-config}, Qt/glib/gcrypt/zlib
# sys-root MinGW installes.

set -e

QDIR="$(cd "$(dirname "$0")" && pwd)"
BUILD="$QDIR/qemu-build-win"
CROSS="x86_64-w64-mingw32-"
SYSROOT="/usr/x86_64-w64-mingw32/sys-root/mingw"

rm -rf "$BUILD"
mkdir -p "$BUILD"

export CC="${CROSS}gcc"
export CXX="${CROSS}g++"
export AR="${CROSS}ar"
export NM="${CROSS}nm"
export PKG_CONFIG="${CROSS}pkg-config"
export PKG_CONFIG_LIBDIR="${SYSROOT}/lib/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="${SYSROOT}"
export PKG_CONFIG_PATH="/tmp/mingw-pc:${PKG_CONFIG_PATH:-}"
export PATH="${SYSROOT}/bin:$PATH"

# gcrypt.pc minimal (sinon configure detruit gcrypt)
mkdir -p /tmp/mingw-pc
cat > /tmp/mingw-pc/gcrypt.pc <<EOF
prefix=${SYSROOT}
libdir=\${prefix}/lib
includedir=\${prefix}/include
Name: gcrypt
Description: GNU cryptographic library
Version: 1.10.1
Libs: -L\${libdir} -lgcrypt
Libs.private: -lgpg-error
Cflags: -I\${includedir}
EOF

flags="$("$QDIR/configure" --help | perl -ne 'print if s/^  ([a-z][\w-]*) .*/\1/' | tail -n +2 | awk '{print "--disable-"$1}' ORS=' ')"

cd "$BUILD"
"$QDIR/configure" \
  --cross-prefix="${CROSS}" \
  --target-list="xtensa-softmmu,riscv32-softmmu" \
  --extra-cflags="-fPIC" \
  --disable-slirp $flags --enable-tcg \
  --enable-system --disable-werror \
  --enable-debug --enable-debug-info \
  --enable-gcrypt --enable-slirp \
  >/tmp/qemu-win-configure.out 2>&1

# Compile tous les objets ; les links d'exes échouent sur
# __imp_slirp_* et les tests slirp -> on ignore, on garde les .obj
ninja "-j$(nproc)" || true

# Aliases : net_slirp.c.obj reference __imp_slirp_X (dllimport) alors que slirp
# est liée en statique dans la DLL. On definit __imp_X = &X.
x86_64-w64-mingw32-nm subprojects/slirp/libslirp.a.p/*.obj \
  | grep -E " [TDB] slirp_" | sed 's/.* \([TDB]\) \(slirp_[a-zA-Z0-9_]*\)/\2/' | sort -u \
  > /tmp/slirp-syms.txt
{
  echo '#include <stddef.h>'
  while read s; do echo "extern void $s(void); void *__imp_$s = (void*)&$s;"; done < /tmp/slirp-syms.txt
} > /tmp/slirp-imp.c
x86_64-w64-mingw32-gcc -c /tmp/slirp-imp.c -o /tmp/slirp-imp.o

for arch in xtensa riscv32; do
  python3 - "$arch" <<'EOF'
import subprocess, re, sys
arch = sys.argv[1]
cmd = subprocess.run(['ninja','-t','commands',f'qemu-system-{arch}.exe'],
                     capture_output=True, text=True, check=True).stdout.strip().split('\n')[-1]
c = cmd.replace(f'-o qemu-system-{arch}.exe', f'-shared -o libqemu-{arch}.dll')
c = c.replace('-Wl,--subsystem,console', '')
c = re.sub(r'\s[\w./\-]*system_main\.c\.obj', '', c)
c = c.replace('-Wl,--end-group',
              '/tmp/slirp-imp.o '
              '-L/usr/x86_64-w64-mingw32/sys-root/mingw/lib -lgcrypt -lgpg-error '
              '-Wl,--allow-multiple-definition '
              f'-Wl,--export-all-symbols -Wl,--out-implib,libqemu-{arch}.dll.a '
              '-Wl,--end-group')
open(f'/tmp/link-{arch}.cmd','w').write(c+'\n')
EOF
  sh "/tmp/link-${arch}.cmd" > "/tmp/link-${arch}.log"
done

echo "=== RESULTAT ==="
file libqemu-xtensa.dll libqemu-riscv32.dll
ls -la libqemu-xtensa.dll libqemu-riscv32.dll
