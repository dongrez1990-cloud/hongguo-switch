#!/usr/bin/env bash
# Build the Switch portlibs that wiliwili needs but that this machine cannot get
# from devkitPro's server.
#
# Background: pkg.devkitpro.org is blocked here by Cloudflare (403 "Attention
# Required!" on every path, including the .db index), so `dkp-pacman -S` cannot
# fetch anything. The prebuilt wiliwili release only carries ffmpeg/libmpv/
# libass/dav1d/libuam/nspmini, which leaves four libraries missing from
# switch-libmpv's static link line (see mpv.pc Libs.private):
#
#     -lmbedtls -lmbedx509 -lmbedcrypto   -> switch-mbedtls      (built here)
#     -lfribidi                            -> switch-libfribidi  (built here)
#     -llua5.1                             -> switch-liblua51    (built here)
#     -lplacebo                            -> switch-libplacebo  (see NOTE below)
#
# Recipes are transcribed from devkitPro/pacman-packages (switch/mbedtls,
# switch/libfribidi, switch/lua51) fetched via the GitHub Contents API.
#
# NOTE on -lplacebo: libplacebo is NOT a declared dependency of switch-libmpv and
# is not shipped in it either, so it is most likely vendored inside libmpv.a via
# the wiliwili mpv.patch. We do not build it here; if the final link complains
# about placebo symbols we will revisit.
#
# Usage: build-missing-portlibs.sh <ascii-root> <srcdir> <pkgdir>
#   <ascii-root>  ASCII path to the project (msys2/gcc mangle non-ASCII args)
#   <srcdir>      holds the pre-downloaded source tarballs
#   <pkgdir>      holds the pre-downloaded .pkg.tar.* files

set -uo pipefail

ROOT="${1:?ascii root required}"
SRC="${2:?srcdir required}"
PKGDIR="${3:?pkgdir required}"
LOGDIR="$ROOT/build-logs"
mkdir -p "$LOGDIR"

# ---------------------------------------------------------------------------
# Environment. devkitPro normally supplies this via /opt/devkitpro/switchvars.sh,
# but that file ships in dkp-toolchain-vars, which we could not install, so we
# reconstruct the equivalent variables here. -mtp=soft and the armv8 arch flags
# MUST match how the prebuilt .a files were compiled or the ABI will not line up.
# ---------------------------------------------------------------------------
export DEVKITPRO=/opt/devkitpro
export PORTLIBS_PREFIX="$DEVKITPRO/portlibs/switch"
export PATH="$DEVKITPRO/tools/bin:$DEVKITPRO/devkitA64/bin:$PORTLIBS_PREFIX/bin:$PATH"

ARCH="-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIC"
# libnx/include must be on the include path: the mbedtls patch pulls in
# <switch.h> for csrngGetRandomBytes. devkitPro's own switchvars.sh adds this;
# since we had to reconstruct those variables we must not forget it.
INC="-I$PORTLIBS_PREFIX/include -I$DEVKITPRO/libnx/include"
export CPPFLAGS="-D__SWITCH__ -D_GNU_SOURCE $INC"
export CFLAGS="-O2 -Wall $ARCH -D__SWITCH__ -D_GNU_SOURCE $INC"
export CXXFLAGS="$CFLAGS -std=gnu++17"
export LDFLAGS="-L$PORTLIBS_PREFIX/lib -L$DEVKITPRO/libnx/lib"
export PKG_CONFIG_PATH="$PORTLIBS_PREFIX/lib/pkgconfig"
export PKG_CONFIG_LIBDIR="$PORTLIBS_PREFIX/lib/pkgconfig"
export JOBS="$(nproc 2>/dev/null || echo 4)"

echo "DEVKITPRO=$DEVKITPRO"
echo "PORTLIBS_PREFIX=$PORTLIBS_PREFIX"
echo "JOBS=$JOBS"
echo "srcdir=$SRC"
echo "pkgdir=$PKGDIR"
echo ""

FAILED=()
ok()   { echo ">>> OK   $1"; }
bad()  { echo ">>> FAIL $1"; FAILED+=("$1"); }

# Each stage takes minutes, so allow re-running just the part that failed.
SKIP_LUA="${SKIP_LUA:-0}"
SKIP_MBEDTLS="${SKIP_MBEDTLS:-0}"
SKIP_FRIBIDI="${SKIP_FRIBIDI:-0}"
SKIP_PACMAN="${SKIP_PACMAN:-0}"
echo "skip flags: LUA=$SKIP_LUA MBEDTLS=$SKIP_MBEDTLS FRIBIDI=$SKIP_FRIBIDI PACMAN=$SKIP_PACMAN"

# ===========================================================================
# Stage 1: lua 5.1.5 -> liblua5.1.a
#   Smallest and most self-contained; a good first check that the cross
#   toolchain works at all. Compiled by hand rather than via lua's Makefile so
#   we control exactly which TUs land in the archive (lua.c/luac.c are the
#   standalone interpreter and compiler and must be excluded).
# ===========================================================================
echo "=================== STAGE 1: lua 5.1.5 ==================="
if [ "$SKIP_LUA" = "1" ]; then echo "SKIPPED"; else
(
  cd "$SRC" || exit 1
  rm -rf lua-5.1.5
  tar xzf lua-5.1.5.tar.gz || exit 1
  cd lua-5.1.5/src || exit 1
  OBJS=""
  for f in *.c; do
    case "$f" in lua.c|luac.c) continue;; esac
    aarch64-none-elf-gcc $CFLAGS $CPPFLAGS -c "$f" -o "${f%.c}.o" || { echo "compile failed: $f"; exit 1; }
    OBJS="$OBJS ${f%.c}.o"
  done
  aarch64-none-elf-ar rcu liblua5.1.a $OBJS || exit 1
  aarch64-none-elf-ranlib liblua5.1.a || exit 1
  install -d "$PORTLIBS_PREFIX/include/lua5.1" "$PORTLIBS_PREFIX/lib"
  install -m 644 lua.h luaconf.h lualib.h lauxlib.h "$PORTLIBS_PREFIX/include/lua5.1/"
  install -m 644 liblua5.1.a "$PORTLIBS_PREFIX/lib/"
  echo "members:"; aarch64-none-elf-ar t liblua5.1.a | tr '\n' ' '; echo
) > "$LOGDIR/stage1-lua.log" 2>&1
if [ $? -eq 0 ] && [ -f "$PORTLIBS_PREFIX/lib/liblua5.1.a" ]; then
  ok "lua51 -> $(stat -c%s "$PORTLIBS_PREFIX/lib/liblua5.1.a" 2>/dev/null) bytes"
else
  tail -25 "$LOGDIR/stage1-lua.log"; bad "lua51"
fi
fi

# ===========================================================================
# Stage 2: mbedtls 2.28.10 -> libmbedtls/mbedx509/mbedcrypto
#   The patch is mandatory: it adds mbedtls_hardware_poll() backed by libnx
#   csrngGetRandomBytes (Horizon has no /dev/urandom) and fixes net_sockets.c.
# ===========================================================================
echo ""
echo "=================== STAGE 2: mbedtls 2.28.10 ==================="
if [ "$SKIP_MBEDTLS" = "1" ]; then echo "SKIPPED"; else
(
  cd "$SRC" || exit 1
  rm -rf mbedtls-2.28.10
  tar xjf mbedtls-2.28.10.tar.bz2 || exit 1
  cd mbedtls-2.28.10 || exit 1

  # No patch(1) in this msys2 install; git apply handles a plain unified diff fine.
  git apply --verbose "$SRC/mbedtls-2.28.10.patch" || { echo "PATCH FAILED"; exit 1; }

  # The upstream recipe uses `scripts/config.pl set/unset`. In mbedtls 2.28.10
  # that file is only a 409-byte Perl shim that re-execs scripts/config.py, which
  # needs python3/python -- neither exists on this machine, so it dies with
  # "python3: No such file or directory". Edit include/mbedtls/config.h directly:
  # every one of these knobs is a single commented/uncommented #define.
  CFG=include/mbedtls/config.h
  sed -i 's|^//#define MBEDTLS_ENTROPY_HARDWARE_ALT|#define MBEDTLS_ENTROPY_HARDWARE_ALT|'   "$CFG" || exit 1
  sed -i 's|^//#define MBEDTLS_NO_PLATFORM_ENTROPY|#define MBEDTLS_NO_PLATFORM_ENTROPY|'     "$CFG" || exit 1
  sed -i 's|^#define MBEDTLS_SELF_TEST|//#define MBEDTLS_SELF_TEST|'                        "$CFG" || exit 1
  echo "--- config.h after edits ---"
  grep -nE '^#define MBEDTLS_(ENTROPY_HARDWARE_ALT|NO_PLATFORM_ENTROPY|SELF_TEST)' "$CFG" || true
  # Fail loudly if any edit silently missed, rather than building a wrong lib.
  grep -qE '^#define MBEDTLS_ENTROPY_HARDWARE_ALT' "$CFG" || { echo "EDIT FAILED: ENTROPY_HARDWARE_ALT"; exit 1; }
  grep -qE '^#define MBEDTLS_NO_PLATFORM_ENTROPY'  "$CFG" || { echo "EDIT FAILED: NO_PLATFORM_ENTROPY"; exit 1; }
  grep -qE '^#define MBEDTLS_SELF_TEST'            "$CFG" && { echo "EDIT FAILED: SELF_TEST still on"; exit 1; }
  echo "config.h edits verified"

  aarch64-none-elf-cmake \
    -DCMAKE_INSTALL_PREFIX="$PORTLIBS_PREFIX" \
    -DCMAKE_C_FLAGS="$CFLAGS $CPPFLAGS -fzero-init-padding-bits=unions" \
    -DZLIB_ROOT="$PORTLIBS_PREFIX" \
    -DENABLE_ZLIB_SUPPORT=TRUE -DENABLE_TESTING=FALSE -DENABLE_PROGRAMS=FALSE \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    . || exit 1
  make -j"$JOBS" || exit 1
  make install || exit 1
) > "$LOGDIR/stage2-mbedtls.log" 2>&1
if [ $? -eq 0 ] && [ -f "$PORTLIBS_PREFIX/lib/libmbedtls.a" ]; then
  ok "mbedtls"
else
  tail -30 "$LOGDIR/stage2-mbedtls.log"; bad "mbedtls"
fi
fi

# ===========================================================================
# Stage 3: fribidi 1.0.12 -> libfribidi.a
#   GitHub release tarballs carry no pre-generated configure, so autogen.sh
#   (autoreconf) must run. The upstream recipe also strips bin/ from SUBDIRS
#   because the fribidi CLI tools clash with getopt on this target.
#   fribidi's configure.ac uses GTK_DOC_CHECK; if gtkdocize is absent autoreconf
#   dies on an undefined macro, so we strip that check as a fallback.
# ===========================================================================
echo ""
echo "=================== STAGE 3: fribidi 1.0.12 ==================="
if [ "$SKIP_FRIBIDI" = "1" ]; then echo "SKIPPED"; else
(
  cd "$SRC" || exit 1
  rm -rf fribidi-1.0.12
  tar xJf fribidi-1.0.12.tar.xz || exit 1
  cd fribidi-1.0.12 || exit 1

  sed '/^SUBDIRS/ s/bin //' -i Makefile.am || exit 1

  # autogen.sh does its own `autoreconf` AND then runs a bare `configure` with no
  # arguments, which hunts for the host's plain `gcc`/`cc`/`cl.exe` and dies with
  # "no acceptable C compiler found in $PATH". NOCONFIGURE=1 tells the standard
  # autogen template to stop after autoreconf; we then drive configure ourselves.
  if ! NOCONFIGURE=1 ./autogen.sh; then
    echo "--- autogen.sh returned non-zero; falling back to autoreconf -fi ---"
    sed -i '/GTK_DOC_CHECK/d' configure.ac
    autoreconf -fi || exit 1
  fi

  # Passing the tools as configure *arguments* (not just env vars) makes autoconf
  # skip its compiler search entirely -- with --host it otherwise insists on
  # resolving a prefixed name itself and gives up.
  ./configure \
    --host=aarch64-none-elf \
    --prefix="$PORTLIBS_PREFIX" \
    --disable-shared --enable-static \
    CC=aarch64-none-elf-gcc \
    AR=aarch64-none-elf-ar \
    RANLIB=aarch64-none-elf-ranlib \
    STRIP=aarch64-none-elf-strip \
    || exit 1
  make -j"$JOBS" || exit 1
  make install || exit 1
) > "$LOGDIR/stage3-fribidi.log" 2>&1
if [ $? -eq 0 ] && [ -f "$PORTLIBS_PREFIX/lib/libfribidi.a" ]; then
  ok "fribidi"
else
  tail -30 "$LOGDIR/stage3-fribidi.log"; bad "fribidi"
fi
fi

# ===========================================================================
# Stage 4: install the pre-downloaded packages.
#   -dd skips dependency resolution: switch-liblua51 is now satisfied by the
#   archive we built in stage 1, but pacman has no record of it.
# ===========================================================================
echo ""
echo "=================== STAGE 4: pacman -U ==================="
if [ "$SKIP_PACMAN" = "1" ]; then echo "SKIPPED"; else
(
  PKGS=()
  for f in "$PKGDIR"/*.pkg.tar.zst "$PKGDIR"/*.pkg.tar.xz; do
    [ -f "$f" ] || continue
    case "$f" in
      *switch-ffmpeg*|*switch-libmpv_deko3d*|*libuam*|*switch-libass*|*switch-dav1d*|*switch-nspmini*|*hacBrewPack*) PKGS+=("$f");;
    esac
  done
  echo "installing: ${PKGS[@]}"
  pacman -U --noconfirm --needed -dd "${PKGS[@]}"
) > "$LOGDIR/stage4-pacman.log" 2>&1
if [ $? -eq 0 ]; then
  ok "pacman -U"
else
  tail -30 "$LOGDIR/stage4-pacman.log"; bad "pacman -U"
fi
fi

# ===========================================================================
# Verification
# ===========================================================================
echo ""
echo "=================== VERIFY ==================="
MISSING=()
for probe in \
  "$PORTLIBS_PREFIX/lib/libmpv.a" \
  "$PORTLIBS_PREFIX/lib/libavcodec.a" \
  "$PORTLIBS_PREFIX/lib/libavformat.a" \
  "$PORTLIBS_PREFIX/lib/libavutil.a" \
  "$PORTLIBS_PREFIX/lib/libswscale.a" \
  "$PORTLIBS_PREFIX/lib/libavfilter.a" \
  "$PORTLIBS_PREFIX/lib/libswresample.a" \
  "$PORTLIBS_PREFIX/lib/libmbedtls.a" \
  "$PORTLIBS_PREFIX/lib/libmbedx509.a" \
  "$PORTLIBS_PREFIX/lib/libmbedcrypto.a" \
  "$PORTLIBS_PREFIX/lib/libdav1d.a" \
  "$PORTLIBS_PREFIX/lib/libass.a" \
  "$PORTLIBS_PREFIX/lib/libfribidi.a" \
  "$PORTLIBS_PREFIX/lib/liblua5.1.a" \
  "$PORTLIBS_PREFIX/lib/libuam.a" \
  "$PORTLIBS_PREFIX/lib/libdeko3d.a" \
  "$PORTLIBS_PREFIX/lib/libharfbuzz.a" \
  "$PORTLIBS_PREFIX/lib/libfreetype.a" \
  "$PORTLIBS_PREFIX/include/mpv/client.h" \
  "$PORTLIBS_PREFIX/include/mpv/render_dk3d.h" \
  "$PORTLIBS_PREFIX/lib/pkgconfig/mpv.pc"
do
  if [ -e "$probe" ]; then echo "OK   $probe"; else echo "MISS $probe"; MISSING+=("$probe"); fi
done

echo ""
echo "=== placebo check: does libmpv.a actually need libplacebo? ==="
if [ -f "$PORTLIBS_PREFIX/lib/libmpv.a" ]; then
  nm -u "$PORTLIBS_PREFIX/lib/libmpv.a" 2>/dev/null | grep -ci placebo || echo "0 placebo undefined refs"
fi

echo ""
if [ ${#FAILED[@]} -eq 0 ] && [ ${#MISSING[@]} -eq 0 ]; then
  echo "ALL STAGES OK"
else
  echo "FAILED STAGES: ${FAILED[*]:-none}"
  echo "MISSING FILES: ${MISSING[*]:-none}"
fi
