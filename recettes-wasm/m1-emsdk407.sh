#!/bin/bash
# =====================================================================
# M1 POC C — chaîne QEMU-wasm sous emsdk 4.0.7 (natif hôte, idempotent) :
#   autotools user-space -> meson -> déps (zlib libffi resstub glib pixman)
#   -> QEMU 10.1 wasm (out-tree neuf) -> /tmp/opencode/qemu-build-407
# Recette ktock (Containerfile) transposée ; tarballs /tmp/opencode/wasm-src.
# Chaque étape est sautée si son produit existe déjà (guards sur $T/lib).
# =====================================================================
set -eo pipefail
set +u
source /home/yves/emsdk/emsdk_env.sh
set -uxo pipefail
emcc --version | head -1

export PATH="$HOME/.local/bin:$HOME/.local/opt/bin:$PATH"

TOOLS=/home/yves/.local/opt
DL=/home/yves/tmp/m1-sources
mkdir -p $TOOLS $DL
cd $DL

# ---------- outillage package (idempotent) ----------
# m4 : PAS de build (gnulib 2021 incompatible gcc 16) — l'hôte a /usr/bin/m4
if ! command -v autoconf >/dev/null || ! autoconf --version 2>/dev/null | grep -q 2.72; then
    fetch() { [ -f "$1" ] || curl -L --max-time 300 -o "$1" "$2"; }
    fetch autoconf-2.72.tar.gz https://ftp.gnu.org/gnu/autoconf/autoconf-2.72.tar.gz
    fetch automake-1.17.tar.gz https://ftp.gnu.org/gnu/automake/automake-1.17.tar.gz
    fetch libtool-2.5.4.tar.gz https://ftp.gnu.org/gnu/libtool/libtool-2.5.4.tar.gz
    build_and_install() {
        local t=$1
        local d=$(tar tf $t | head -1 | cut -d/ -f1)
        rm -rf $d && tar xf $t
        cd $d && ./configure --prefix=$TOOLS >/dev/null 2>$TOOLS/$(echo $t | cut -d- -f1).cfg.err
        make -j8 >/dev/null
        make install >/dev/null
        cd ..
    }
    export M4=/usr/bin/m4
    build_and_install autoconf-2.72.tar.gz
    build_and_install automake-1.17.tar.gz
    build_and_install libtool-2.5.4.tar.gz
fi
autoconf --version | head -1

if ! command -v meson >/dev/null || [ "$(meson --version)" != "1.5.0" ]; then
    pip3 install --quiet --user --break-system-packages 'meson==1.5.0' \
      || pip3 install --quiet --user 'meson==1.5.0'
fi
meson --version

# ---------- variables ktock ----------
W=/home/yves/tmp/qemu-wasm-407
T=$W/target
B=$W/builds
SRC=/tmp/opencode/wasm-src
QSRC=/tmp/opencode/qemu-101
mkdir -p $T $B

export CFLAGS="-O2 -matomics -mbulk-memory -DNDEBUG -sWASM_BIGINT -DWASM_BIGINT -pthread -sMALLOC=mimalloc -sASYNCIFY=1"
export CXXFLAGS="$CFLAGS"
export LDFLAGS="-L$T/lib -O2"
export CPATH="$T/include"
export PKG_CONFIG_PATH="$T/lib/pkgconfig"
export PKG_CONFIG_LIBDIR="$T/lib/pkgconfig"
export EM_PKG_CONFIG_PATH="$PKG_CONFIG_PATH"
export CHOST="wasm32-unknown-linux"

cd $B

# ---------- zlib 1.3.1 ----------
if [ ! -f $T/lib/libz.a ]; then
    rm -rf zlib && mkdir zlib && tar xzf $SRC/zlib-1.3.1.tar.gz -C zlib --strip-components=1
    cd zlib
    emconfigure ./configure --prefix=$T --static
    emmake make -j8 install
    cd ..
fi

# ---------- libffi (pin ktock adbcf2b) ----------
if [ ! -f $T/lib/libffi.a ]; then
    rm -rf libffi && mkdir libffi && tar xzf $SRC/libffi-adbcf2b.tar.gz -C libffi --strip-components=1
    cd libffi
    autoreconf -fiv
    LDFLAGS="$LDFLAGS -sEXPORTED_RUNTIME_METHODS='getTempRet0,setTempRet0'" \
    emconfigure ./configure --host=$CHOST --prefix=$T --enable-static --disable-shared \
        --disable-dependency-tracking --disable-builddir --disable-multi-os-directory \
        --disable-raw-api --disable-structs --disable-docs
    emmake make install SUBDIRS='include'
    cd ..
fi

# ---------- stub libresolv ----------
if [ ! -f $T/lib/libresolv.a ]; then
    rm -rf res && mkdir res && cd res
    printf '#include <netdb.h>\nint res_query(const char *name, int class, int type, unsigned char *dest, int len)\n{\n    h_errno = HOST_NOT_FOUND;\n    return -1;\n}\n' > res_query.c
    emcc $CFLAGS -c res_query.c -fPIC -o libresolv.o
    ar rcs $T/lib/libresolv.a libresolv.o
    cd ..
fi

# ---------- glib 2.75.0 (meson cross wasm32) ----------
if [ ! -f $T/lib/libglib-2.0.a ]; then
    rm -rf glib && mkdir glib && tar xJf $SRC/glib-2.75.0.tar.xz -C glib --strip-components=1
    cd glib
    export CFLAGS="-Wno-error=incompatible-function-pointer-types -Wincompatible-function-pointer-types -O2 -matomics -mbulk-memory -DNDEBUG -pthread -sWASM_BIGINT -sMALLOC=mimalloc -sASYNCIFY=1"
    export CXXFLAGS="$CFLAGS"
    cat > $B/emcc-wrap.sh <<'EOF'
#!/bin/bash
set -euo pipefail
old="-Werror=unused-command-line-argument"
new="-Wno-error=unused-command-line-argument"
cmd="$1"; shift
new_args=()
for arg in "$@"; do new_args+=("${arg//$old/$new}"); done
exec "$cmd" "${new_args[@]}"
EOF
    chmod +x $B/emcc-wrap.sh
    cat > $B/cross.meson <<'EOF'
[host_machine]
system = 'emscripten'
cpu_family = 'wasm32'
cpu = 'wasm32'
endian = 'little'

[binaries]
c = ['bash', '/home/yves/tmp/qemu-wasm-407/builds/emcc-wrap.sh', 'emcc']
cpp = ['bash', '/home/yves/tmp/qemu-wasm-407/builds/emcc-wrap.sh', 'em++']
ar = 'emar'
ranlib = 'emranlib'
pkgconfig = ['pkg-config', '--static']
EOF
    meson setup _build --prefix=$T --cross-file=$B/cross.meson --default-library=static \
        --buildtype=release --force-fallback-for=pcre2,gvdb -Dselinux=disabled -Dxattr=false \
        -Dlibmount=disabled -Dnls=disabled -Dtests=false -Dglib_assert=false -Dglib_checks=false \
        -Dlibelf=disabled
    sed -i -E "/#define HAVE_CLOSE_RANGE 1/d; /#define HAVE_EPOLL_CREATE 1/d; /#define HAVE_KQUEUE 1/d; /#define HAVE_POSIX_SPAWN 1/d; /#define HAVE_FALLOCATE 1/d" _build/config.h
    meson install -C _build
    cd ..
fi

# ---------- pixman 0.42.2 (meson, statique) ----------
if [ ! -f $T/lib/libpixman-1.a ]; then
    export CFLAGS="-O2 -matomics -mbulk-memory -DNDEBUG -sWASM_BIGINT -DWASM_BIGINT -pthread -sMALLOC=mimalloc -sASYNCIFY=1"
    export CXXFLAGS="$CFLAGS"
    rm -rf pixman && mkdir pixman && tar xjf $SRC/pixman-0.42.2.tar.bz2 -C pixman --strip-components=1
    cd pixman
    meson setup _build --prefix=$T --cross-file=$B/cross.meson --default-library=static \
        --buildtype=release -Dtests=disabled -Dlibpng=disabled
    meson install -C _build
    cd ..
fi

echo "===== Déps wasm 4.0.7 terminées : $(ls $T/lib) ====="

# ---------- QEMU 10.1 wasm (out-tree neuf) ----------
rm -rf $B/qsrc
cp -a $QSRC $B/qsrc
OUT=$W/out
mkdir -p $OUT && cd $OUT

EXTRA_CFLAGS="-O3 -g -Wno-error=unused-command-line-argument -matomics -mbulk-memory -DNDEBUG -DG_DISABLE_ASSERT -D_GNU_SOURCE -sASYNCIFY=1 -pthread -sPROXY_TO_PTHREAD=1 -sFORCE_FILESYSTEM -sALLOW_TABLE_GROWTH -sINITIAL_MEMORY=2300MB -sWASM_BIGINT -sMALLOC=mimalloc"

if [ ! -f build.ninja ]; then
    emconfigure $B/qsrc/configure --static --target-list=xtensa-softmmu --cpu=wasm32 --cross-prefix= \
        --without-default-features --enable-system --with-coroutine=wasm --enable-tcg-interpreter \
        --extra-cflags="$EXTRA_CFLAGS" --extra-cxxflags="$EXTRA_CFLAGS" \
        --extra-ldflags="-sEXPORTED_RUNTIME_METHODS=getTempRet0,setTempRet0,addFunction,removeFunction,TTY,FS,HEAPU32,HEAP8,HEAPF32 -sEXPORTED_FUNCTIONS=_main,_malloc,_free,_qemu_init,_qemu_main_loop,_qemu_cleanup,_qemu_system_reset_request,_qemu_system_shutdown_request,_qemu_system_vmstop_request,_qemu_picsimlab_register_callbacks,_qemu_picsimlab_set_pin,_qemu_picsimlab_set_apin_mv,_qemu_picsimlab_uart_receive,_qemu_picsimlab_get_internals,_qemu_picsimlab_get_TIOCM,_qemu_picsimlab_flash_dump"
fi
emmake make -j22

# artefacts vers le répertoire du driver 4.0.7
mkdir -p /tmp/opencode/qemu-build-407
JS=$(find $OUT $B/qsrc -maxdepth 4 -name 'qemu-system-xtensa.js' 2>/dev/null | head -1)
echo "artefact emlink: $JS"
[ -n "$JS" ] && cp $(dirname $JS)/qemu-system-xtensa.* /tmp/opencode/qemu-build-407/
ls -la /tmp/opencode/qemu-build-407

echo M1BUILD_OK
