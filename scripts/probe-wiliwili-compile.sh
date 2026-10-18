#!/usr/bin/env bash
# Compile-probe: configure wiliwili for Switch and build with `make -k`, so that a
# single failing translation unit does not hide the rest of the picture.
#
# The point is breadth, not a working binary: the dominant unknown at this stage
# is whether devkitA64's GCC 16 can still compile this C++17 codebase (upstream
# pins an image from 2025, we have a 2026 toolchain), plus whether any vendored
# subproject breaks under CMake 4. Knowing the full error surface in one pass is
# far cheaper than fixing one error per build.
#
# Usage: probe-wiliwili-compile.sh <ascii-root>

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

cd "$SRCDIR" || exit 2

# Path style, carefully:
#   * CMake here is msys2's *msys* build, so it only understands POSIX paths --
#     passing "C:/Users/..." makes it treat that as a RELATIVE path and it fails
#     with a doubled-up source directory. So -S/-B must stay in /c/... form.
#   * aarch64-none-elf-gcc is a native Windows binary and cannot open "/c/...".
#     That is exactly why the unity build breaks: CMake writes
#       #include "/c/Users/.../library/borealis/library/lib/views/header.cpp"
#     into the generated unity_*.cxx and the compiler cannot find the file.
# Turning the unity build off sidesteps the conflict entirely: ordinary targets are
# compiled with build-dir-relative paths (the earlier probe produced 112 .o files
# that way), only the generated unity batches used absolute paths.

echo "=== CONFIGURE ==="
aarch64-none-elf-cmake -B "$BUILD" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILTIN_NSP=OFF \
    -DPLATFORM_SWITCH=ON \
    -DUSE_DEKO3D=ON \
    -DBRLS_UNITY_BUILD=OFF \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    > "$LOGDIR/probe-configure.log" 2>&1
CFG=$?
echo "configure rc=$CFG"
grep -E 'Found libmpv|Found PkgConfig|CMake Error|Configuring done|Generating done' "$LOGDIR/probe-configure.log" | head -20
if [ $CFG -ne 0 ]; then
    echo "--- configure tail ---"
    tail -30 "$LOGDIR/probe-configure.log"
    exit 1
fi

echo ""
echo "=== BUILD (make -k, keep going past errors) ==="
make -C "$BUILD" -k -j"$(nproc)" wiliwili.nro > "$LOGDIR/probe-build.log" 2>&1
echo "make rc=$?"
LOG="$LOGDIR/probe-build.log"
echo "log lines: $(wc -l < "$LOG")"

echo ""
echo "=== error summary ==="
grep -oE '^[^ ]+\.(cpp|cc|c|h):[0-9]+:[0-9]+: error: .*' "$LOG" | sed 's/.*error: //' | sort | uniq -c | sort -rn | head -25

echo ""
echo "=== distinct error kinds ==="
grep -c 'error:' "$LOG" || true
echo "--- first 15 raw errors ---"
grep -n 'error:' "$LOG" | head -15

echo ""
echo "=== undefined references at link time (which libs are still missing) ==="
grep -oE 'undefined reference to `[^'"'"']+'"'"'' "$LOG" | sed "s/undefined reference to //" | sort -u | head -40

echo ""
echo "=== object files produced ==="
find "$BUILD" -name '*.o' 2>/dev/null | wc -l

echo ""
echo "=== warnings count ==="
grep -c 'warning:' "$LOG" || true

echo ""
echo "=== link step reached? ==="
grep -nE 'Linking CXX executable|elf2nro|nacptool' "$LOG" | head -10
