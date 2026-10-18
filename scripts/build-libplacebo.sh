#!/usr/bin/env bash
# Build switch-libplacebo and finish the wiliwili link.
#
# libplacebo is the last missing piece of the static link chain. switch-libmpv's
# mpv.pc lists -lplacebo in Libs.private and the object files genuinely reference
# it (the symbols are prefixed `pl_`, e.g. pl_buf_create, pl_color_space_srgb --
# note NOT `placebo`, which is what made an earlier grep produce a false
# negative), but xfangfang's repacked prebuilt omits the library and devkitPro's
# switch-libplacebo is Cloudflare-403'd from here. So we build it.
#
# Recipe transcribed from devkitPro/pacman-packages switch/libplacebo (6.338.2).
# Two substitutions are forced by this machine:
#   * `dkp-meson-scripts` (/opt/devkitpro/meson-cross.sh + crossfile.txt) ships in
#     a 403'd package, so the crossfile is written by hand below.
#   * the source is taken from the GitHub mirror. code.videolan.org sits behind an
#     Anubis anti-bot challenge that answers HTTP 200 with an HTML "Making sure
#     you're not a bot!" page, which is not a tarball.
#
# Usage: build-libplacebo.sh <ascii-root>

set -uo pipefail

ROOT="${1:?ascii root required}"
SRC="$ROOT/third_party/src"
LOGDIR="$ROOT/build-logs"
mkdir -p "$LOGDIR"

# libplacebo is built OUTSIDE the project tree, in a genuinely ASCII directory.
# Reason: $ROOT is reached through a directory junction, and meson/ninja resolve
# it to its real target, so generated command lines come out carrying the
# non-ASCII workspace path (observed as /d/<non-ASCII>/hongguo-switch/...). That
# is exactly the shape aarch64-none-elf-gcc mangles in its arguments. Building in
# the session temp dir keeps every path pure ASCII end to end.
LPBUILD="<ascii-scratch>/lpbuild"

export DEVKITPRO=/opt/devkitpro
export PORTLIBS_PREFIX="$DEVKITPRO/portlibs/switch"
export PATH="$DEVKITPRO/tools/bin:$DEVKITPRO/devkitA64/bin:$PORTLIBS_PREFIX/bin:/usr/bin:$PATH"
export PKG_CONFIG_PATH="$PORTLIBS_PREFIX/lib/pkgconfig"
export PKG_CONFIG_LIBDIR="$PORTLIBS_PREFIX/lib/pkgconfig"
export JOBS="$(nproc 2>/dev/null || echo 4)"

PLV=6.338.2
VKV=1.3.283
PLTAR="$SRC/v$PLV.tar.gz"
VKTAR="$SRC/v$VKV.tar.gz"

# ===========================================================================
# 0) Restore -lplacebo in mpv.pc.
#    A previous round removed it on a false negative and the fix could not be
#    reverted without elevation. Do it first: every later step links against
#    this file, and without it the linker silently omits a real dependency.
# ===========================================================================
echo "=================== 0) restore -lplacebo in mpv.pc ==================="
PC="$PORTLIBS_PREFIX/lib/pkgconfig/mpv.pc"
[ -f "$PC" ] || { echo "FATAL: mpv.pc missing at $PC"; exit 2; }
if grep -q -- '-lplacebo' "$PC"; then
  echo "already present, nothing to do"
else
  sed -i 's/\( -llua5\.1\)/ -lplacebo\1/' "$PC"
  if grep -q -- '-lplacebo' "$PC"; then
    echo "restored"; grep -E '^Libs.private:' "$PC"
  else
    echo "FATAL: could not restore -lplacebo (pattern not found)"; exit 2
  fi
fi

# ===========================================================================
# 1) meson / ninja, from the msys repo (the msys mirror IS reachable; only the
#    dkp-* repos 403).
# ===========================================================================
echo ""
echo "=================== 1) ensure meson + ninja + jinja2 ==================="
if command -v meson >/dev/null 2>&1 && command -v ninja >/dev/null 2>&1; then
  echo "already installed: $(meson --version) / $(ninja --version)"
else
  # The dkp-* sync db failures are expected and harmless for msys packages.
  pacman -Sy --noconfirm >/dev/null 2>&1 || true
  pacman -S --needed --noconfirm python meson ninja 2>&1 | tail -20
fi
echo "meson: $(command -v meson || echo MISSING)  $(meson --version 2>/dev/null)"
echo "ninja: $(command -v ninja || echo MISSING)  $(ninja --version 2>/dev/null)"

# libplacebo generates src/shaders/*.c with tools/glsl_preproc, which imports
# jinja2. Without it the build dies at [13/52] with
#   ModuleNotFoundError: No module named 'jinja2'
if python -c 'import jinja2' >/dev/null 2>&1; then
  echo "jinja2: present ($(python -c 'import jinja2; print(jinja2.__version__)'))"
else
  echo "jinja2: installing..."
  pacman -S --needed --noconfirm python-jinja 2>&1 | tail -10
  if ! python -c 'import jinja2' >/dev/null 2>&1; then
    echo "pacman did not provide it; trying pip"
    python -m pip install --quiet jinja2 2>&1 | tail -10 || true
  fi
  python -c 'import jinja2; print("jinja2 OK", jinja2.__version__)' 2>&1 || echo "jinja2 STILL MISSING"
fi

# ===========================================================================
# 2) Unpack sources; libplacebo expects Vulkan headers copied into src/include.
# ===========================================================================
echo ""
echo "=================== 2) stage sources into an ASCII-only build dir ==================="
echo "build dir: $LPBUILD"
(
  rm -rf "$LPBUILD"
  mkdir -p "$LPBUILD"
  cd "$LPBUILD" || exit 1
  tar xzf "$PLTAR" || { echo "bad libplacebo tarball"; exit 1; }
  tar xzf "$VKTAR" || { echo "bad Vulkan-Headers tarball"; exit 1; }
  # GitHub's archive of a tag is named <repo>-<version-without-v>.
  [ -d "libplacebo-$PLV" ] && mv "libplacebo-$PLV" "libplacebo-v$PLV"
  [ -d "Vulkan-Headers-$VKV" ] || { echo "vulkan headers dir name unexpected:"; ls -d Vulkan*; exit 1; }
  cp -r "Vulkan-Headers-$VKV"/include/* "libplacebo-v$PLV/src/include/"
  echo "libplacebo src: $(find "libplacebo-v$PLV" -type f | wc -l) files"
  echo "vulkan headers copied: $(find "libplacebo-v$PLV/src/include/vulkan" -type f 2>/dev/null | wc -l)"
) > "$LOGDIR/libplacebo-unpack.log" 2>&1
if [ -d "$LPBUILD/libplacebo-v$PLV/src/include/vulkan" ]; then
  echo "OK"; grep -E 'libplacebo src|vulkan headers' "$LOGDIR/libplacebo-unpack.log"
else
  echo "FAIL"; tail -20 "$LOGDIR/libplacebo-unpack.log"
fi

# ===========================================================================
# 3) Hand-written cross file, standing in for dkp-meson-scripts.
# ===========================================================================
CROSS="$ROOT/scripts/libplacebo-crossfile.txt"
cat > "$CROSS" <<'EOF'
# Stand-in for devkitPro's meson-cross.sh crossfile (that package is 403'd here).
[binaries]
c        = 'aarch64-none-elf-gcc'
cpp      = 'aarch64-none-elf-g++'
ar       = 'aarch64-none-elf-ar'
strip    = 'aarch64-none-elf-strip'
objcopy  = 'aarch64-none-elf-objcopy'
pkgconfig = 'aarch64-none-elf-pkg-config'

[host_machine]
system     = 'horizon'
cpu_family = 'aarch64'
cpu        = 'cortex-a57'
endian     = 'little'

[properties]
needs_exe_wrapper = true

# -mtp=soft must match how every other Switch static library was compiled or the
# thread-pointer ABI will not line up.
[built-in options]
c_args      = ['-D__SWITCH__', '-D_GNU_SOURCE', '-O2', '-march=armv8-a+crc+crypto', '-mtune=cortex-a57', '-mtp=soft', '-fPIC', '-I/opt/devkitpro/portlibs/switch/include', '-I/opt/devkitpro/libnx/include']
c_link_args = ['-specs=/opt/devkitpro/libnx/switch.specs', '-L/opt/devkitpro/portlibs/switch/lib', '-L/opt/devkitpro/libnx/lib']
cpp_args      = ['-D__SWITCH__', '-D_GNU_SOURCE', '-O2', '-march=armv8-a+crc+crypto', '-mtune=cortex-a57', '-mtp=soft', '-fPIC', '-I/opt/devkitpro/portlibs/switch/include', '-I/opt/devkitpro/libnx/include']
cpp_link_args = ['-specs=/opt/devkitpro/libnx/switch.specs', '-L/opt/devkitpro/portlibs/switch/lib', '-L/opt/devkitpro/libnx/lib']
EOF
echo ""
echo "wrote crossfile: $CROSS"

# ===========================================================================
# 4) Build and install libplacebo.
# ===========================================================================
echo ""
echo "=================== 4) meson build libplacebo ==================="
(
  set -e
  cd "$LPBUILD/libplacebo-v$PLV"
  rm -rf build
  # Options exactly as the official recipe: every GPU backend, shader compiler
  # and optional dependency is off. mpv on Switch gets its output path from
  # deko3d; libplacebo supplies the colour/filter maths only.
  # -Ddefault_library=static is essential: without it meson tries to produce
  # libplacebo.so.338, which for a bare-metal Horizon target has no business
  # existing and fails at the very last link with undefined strcmp/free (a
  # shared-library link does not pull newlib in the way the specs expect).
  # devkitPro's meson-cross.sh supplies this; our hand-written crossfile must too.
  meson setup build --cross-file "$CROSS" \
    --prefix="$PORTLIBS_PREFIX" \
    --buildtype=release \
    -Ddefault_library=static \
    -Dopengl=disabled -Dgl-proc-addr=disabled \
    -Dd3d11=disabled -Dglslang=disabled -Dshaderc=disabled \
    -Ddovi=disabled -Dlibdovi=disabled \
    -Dlcms=disabled -Dvulkan=disabled \
    -Ddemos=false -Dtests=false
  meson compile -C build
  meson install -C build
) > "$LOGDIR/build-libplacebo.log" 2>&1

if [ -f "$PORTLIBS_PREFIX/lib/libplacebo.a" ]; then
  echo ">>> OK libplacebo.a -> $(stat -c%s "$PORTLIBS_PREFIX/lib/libplacebo.a") bytes"
else
  echo ">>> FAIL libplacebo -- last 40 lines:"
  tail -40 "$LOGDIR/build-libplacebo.log"
fi

echo ""
echo "=== pkgconfig ==="
ls -1 "$PORTLIBS_PREFIX/lib/pkgconfig" 2>/dev/null | head -20

# ===========================================================================
# 5) Full wiliwili build.
# ===========================================================================
echo ""
echo "=================== 5) wiliwili full build ==================="
rm -rf "$ROOT/upstream/wiliwili/cmake-build-switch"
bash "$ROOT/scripts/probe-wiliwili-compile.sh" "$ROOT"
