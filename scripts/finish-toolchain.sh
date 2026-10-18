#!/usr/bin/env bash
# Close the last two gaps in the Switch portlib chain and then probe whether
# wiliwili itself still compiles under this toolchain.
#
# Gap 1 -- mbedtls: the three archives were built successfully, but `make install`
#   never ran because GNU Make 4.4 aborted on CMake's generated
#   `compiler_depend.make` ("multiple targets match"). The artifacts are complete,
#   so install them straight from the build tree instead of fighting make.
#
# Gap 2 -- curl: cpr (wiliwili's HTTP layer) requires libcurl, which exists
#   nowhere on this machine. devkitPro's own switch-curl recipe is curl 7.69.1
#   built against libnx's TLS backend, and wiliwili ships the matching patch.
#   The patch edits configure.ac, so buildconf/autoreconf must regenerate
#   configure before it will accept --with-libnx.
#
# Usage: finish-toolchain.sh <ascii-root>

set -uo pipefail

ROOT="${1:?ascii root required}"
SRC="$ROOT/third_party/src"
WS="$ROOT/upstream/wiliwili"
LOGDIR="$ROOT/build-logs"
mkdir -p "$LOGDIR"

export DEVKITPRO=/opt/devkitpro
export PORTLIBS_PREFIX="$DEVKITPRO/portlibs/switch"
export PATH="$DEVKITPRO/tools/bin:$DEVKITPRO/devkitA64/bin:$PORTLIBS_PREFIX/bin:$PATH"
export PKG_CONFIG_PATH="$PORTLIBS_PREFIX/lib/pkgconfig"
export PKG_CONFIG_LIBDIR="$PORTLIBS_PREFIX/lib/pkgconfig"
INC="-I$PORTLIBS_PREFIX/include -I$DEVKITPRO/libnx/include"
export CPPFLAGS="-D__SWITCH__ -D_GNU_SOURCE $INC"
export CFLAGS="-O2 -Wall -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIC -D__SWITCH__ -D_GNU_SOURCE $INC"
# Must be defined before use: this script runs under `set -u`, and the curl build
# line below appends to LDFLAGS. build-missing-portlibs.sh exported it, this one
# has to as well or the reference aborts the script.
export LDFLAGS="-L$PORTLIBS_PREFIX/lib -L$DEVKITPRO/libnx/lib"
export JOBS="$(nproc 2>/dev/null || echo 4)"

# ===========================================================================
# 1) Install the already-built mbedtls archives and headers
# ===========================================================================
echo "=================== 1) install mbedtls ==================="
(
  set -e
  cd "$SRC/mbedtls-2.28.10"
  cp library/libmbedcrypto.a library/libmbedx509.a library/libmbedtls.a "$PORTLIBS_PREFIX/lib/"
  mkdir -p "$PORTLIBS_PREFIX/include/mbedtls"
  cp include/mbedtls/*.h "$PORTLIBS_PREFIX/include/mbedtls/"
  if [ -d include/psa ]; then
    mkdir -p "$PORTLIBS_PREFIX/include/psa"
    cp include/psa/*.h "$PORTLIBS_PREFIX/include/psa/"
  fi
  echo "installed:"; ls -1 "$PORTLIBS_PREFIX/lib"/libmbed*.a
) > "$LOGDIR/install-mbedtls.log" 2>&1
if [ -f "$PORTLIBS_PREFIX/lib/libmbedtls.a" ]; then echo ">>> OK mbedtls installed"; else tail -20 "$LOGDIR/install-mbedtls.log"; echo ">>> FAIL mbedtls"; fi

# ===========================================================================
# 2) Build switch-curl (libcurl with the libnx TLS backend)
# ===========================================================================
echo ""
echo "=================== 2) build switch-curl 7.71.1 ==================="
(
  cd "$SRC" || exit 1
  # 7.71.1, NOT 7.69.1: wiliwili's curl.patch was generated against 7.71.1 and its
  # hunks do not apply to the devkitPro recipe's older 7.69.1 sources.
  rm -rf curl-7.71.1
  tar xJf curl-7.71.1.tar.xz || exit 1
  cd curl-7.71.1 || exit 1

  # --ignore-whitespace: the patch carries trailing whitespace that git apply
  # otherwise rejects, and `patch(1)` is not installed on this msys2.
  git apply -p1 --ignore-whitespace "$WS/scripts/switch/curl/curl.patch" \
    || { echo "PATCH FAILED"; exit 1; }

  # The patch touches configure.ac, so configure must be regenerated.
  if [ ! -x buildconf ] || ! ./buildconf; then
    echo "--- buildconf unusable/failed; trying autoreconf -fi ---"
    autoreconf -fi || exit 1
  fi

  # -lnx and the libnx lib dir are required by curl's link tests: gethostbyname()
  # lives in libnx (nm shows `T gethostbyname`), but libnx/switch.specs only sets
  # linker switches and does NOT pull -lnx in automatically. Without this the
  # configure dies with "couldn't find libraries for gethostbyname()".
  # devkitPro's own switchvars.sh supplies exactly this; we are reconstructing it.
  LDFLAGS="-specs=$DEVKITPRO/libnx/switch.specs -L$DEVKITPRO/libnx/lib $LDFLAGS" \
  LIBS="-lnx" \
  ./configure --prefix="$PORTLIBS_PREFIX" --host=aarch64-none-elf \
    --disable-shared --enable-static --disable-ipv6 --disable-unix-sockets \
    --disable-manual --disable-ntlm-wb --disable-threaded-resolver \
    --without-ssl --without-polar-ssl --without-cyassl --without-wolfssl \
    --without-mbedtls \
    --with-libnx --with-default-ssl-backend=libnx \
    CC=aarch64-none-elf-gcc AR=aarch64-none-elf-ar \
    RANLIB=aarch64-none-elf-ranlib STRIP=aarch64-none-elf-strip \
    || exit 1

  make -C lib -j"$JOBS" || exit 1
  make -C lib install || exit 1
  make -C include install || exit 1
  make install-pkgconfigDATA || exit 1
) > "$LOGDIR/build-curl.log" 2>&1
if [ -f "$PORTLIBS_PREFIX/lib/libcurl.a" ]; then
  echo ">>> OK curl -> $(stat -c%s "$PORTLIBS_PREFIX/lib/libcurl.a") bytes"
else
  echo ">>> FAIL curl -- last 30 lines:"; tail -30 "$LOGDIR/build-curl.log"
fi

# ===========================================================================
# 3) Verify the chain
# ===========================================================================
echo ""
echo "=================== 3) verify ==================="
for p in libmpv.a libavcodec.a libavformat.a libavutil.a libswscale.a libavfilter.a \
         libswresample.a libmbedtls.a libmbedx509.a libmbedcrypto.a libdav1d.a libass.a \
         libfribidi.a liblua5.1.a libuam.a libcurl.a; do
  if [ -f "$PORTLIBS_PREFIX/lib/$p" ]; then echo "OK   $p"; else echo "MISS $p"; fi
done
[ -f "$DEVKITPRO/libnx/lib/libdeko3d.a" ] && echo "OK   libdeko3d.a (in libnx/lib)" || echo "MISS libdeko3d.a"

# ===========================================================================
# 4) Probe: how far does wiliwili itself get?
# ===========================================================================
echo ""
echo "=================== 4) wiliwili compile probe ==================="
bash "$ROOT/scripts/probe-wiliwili-compile.sh" "$ROOT"
