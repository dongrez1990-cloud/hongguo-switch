#!/usr/bin/env bash
# Build wiliwili for Nintendo Switch (deko3d variant).
#
# This is the baseline that proves the whole reuse stack works on this machine:
# SDL2/OpenGL-or-deko3d rendering + libmpv/ffmpeg decode + curl/mbedTLS network
# + borealis gamepad focus navigation + NSP packaging.
#
# Differences from upstream scripts/build_switch_deko3d.sh, all forced by this
# machine and documented so the deviation is reviewable:
#   1. It uses `curl -LO` to fetch the prebuilt packages. curl here is broken
#      (schannel SEC_E_NO_CREDENTIALS), so packages are pre-downloaded with Node
#      instead and this script expects them to already be installed.
#   2. It relies on dkp-pacman to install switch-ffmpeg/switch-libmpv/lua51/
#      mbedtls/fribidi. pkg.devkitpro.org is Cloudflare-403'd from here, so those
#      were either installed from the wiliwili release or cross-compiled by
#      build-missing-portlibs.sh.
#   3. It passes CMAKE_TOOLCHAIN_FILE via the devkitA64 cmake wrapper, and
#      CMAKE_POLICY_VERSION_MINIMUM because CMake 4 dropped <3.5 compatibility
#      while some of the vendored subprojects still declare an older minimum.
#
# The source tree must be reached through a pure-ASCII path: aarch64-none-elf-gcc
# mangles non-ASCII arguments, and the workspace lives under D:\<non-ASCII>.
#
# Usage: build-wiliwili-switch.sh <ascii-root>

set -uo pipefail

ROOT="${1:?ascii root required}"
SRCDIR="$ROOT/upstream/wiliwili"
LOGDIR="$ROOT/build-logs"
BUILD="cmake-build-switch"
mkdir -p "$LOGDIR"

export DEVKITPRO=/opt/devkitpro
export PORTLIBS_PREFIX="$DEVKITPRO/portlibs/switch"
export PATH="$DEVKITPRO/tools/bin:$DEVKITPRO/devkitA64/bin:$PORTLIBS_PREFIX/bin:$PATH"
export PKG_CONFIG_PATH="$PORTLIBS_PREFIX/lib/pkgconfig"
export PKG_CONFIG_LIBDIR="$PORTLIBS_PREFIX/lib/pkgconfig"
export JOBS="$(nproc 2>/dev/null || echo 4)"

[ -d "$SRCDIR" ] || { echo "FATAL: no source at $SRCDIR"; exit 2; }
cd "$SRCDIR" || exit 2

echo "=== sanity: the link chain must be complete before we start ==="
MISSING=0
for lib in mpv avcodec avformat avutil swscale avfilter swresample mbedtls mbedx509 \
           mbedcrypto dav1d ass fribidi lua5.1 uam deko3d harfbuzz freetype bz2 png16; do
    if [ ! -f "$PORTLIBS_PREFIX/lib/lib$lib.a" ]; then echo "  MISS lib$lib.a"; MISSING=1; fi
done
[ -f "$PORTLIBS_PREFIX/lib/libnx.a" ] || [ -f "$DEVKITPRO/libnx/lib/libnx.a" ] || echo "  note: libnx.a not in portlibs (comes from libnx package)"
if [ "$MISSING" = "1" ]; then
    echo "FATAL: refusing to configure with an incomplete link chain."
    exit 2
fi
echo "  all portlibs present"
echo ""

# git refuses to read a repo it considers foreign-owned; harmless if HOME is
# read-only, so do not let it abort the build.
git config --global --add safe.directory "$SRCDIR" 2>/dev/null || true

echo "=== cmake configure ===" | tee "$LOGDIR/wiliwili-configure.log"
aarch64-none-elf-cmake -B "$BUILD" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILTIN_NSP=ON \
    -DPLATFORM_SWITCH=ON \
    -DUSE_DEKO3D=ON \
    -DBRLS_UNITY_BUILD=ON \
    -DCMAKE_UNITY_BUILD_BATCH_SIZE=16 \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    >> "$LOGDIR/wiliwili-configure.log" 2>&1
if [ $? -ne 0 ]; then
    echo "CONFIGURE FAILED -- last 40 lines:"
    tail -40 "$LOGDIR/wiliwili-configure.log"
    exit 1
fi
echo "configure OK"
grep -E 'Found libmpv|Found libwebp|Found PkgConfig|Configuring done|Generating done' "$LOGDIR/wiliwili-configure.log" | tail -10
echo ""

echo "=== build wiliwili.nro ===" | tee "$LOGDIR/wiliwili-build.log"
make -C "$BUILD" wiliwili.nro -j"$JOBS" >> "$LOGDIR/wiliwili-build.log" 2>&1
RC=$?
if [ $RC -ne 0 ]; then
    echo "BUILD FAILED (rc=$RC) -- first errors:"
    grep -nE 'error:|Error [0-9]|undefined reference|undefined symbol' "$LOGDIR/wiliwili-build.log" | head -40
    echo ""
    echo "-- last 25 lines --"
    tail -25 "$LOGDIR/wiliwili-build.log"
    exit 1
fi

echo "build OK"
echo ""
echo "=== artifacts ==="
find "$BUILD" -maxdepth 2 \( -name '*.nro' -o -name '*.nsp' -o -name '*.elf' -o -name '*.nacp' \) -printf '%s\t%p\n' 2>/dev/null | sort -rn
echo ""
echo "=== warnings worth looking at ==="
grep -cE 'warning:' "$LOGDIR/wiliwili-build.log" || true
echo "done"
