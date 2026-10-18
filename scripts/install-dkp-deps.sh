#!/usr/bin/env bash
# 安装 wiliwili Switch 构建所需的 devkitPro 包。
#
# 必须在 devkitPro 的 msys2 登录 shell 下运行，且需要解除沙箱对命名管道的限制
# （msys2 二进制起不来时会报 "couldn't create signal pipe, Win32 error 5"）。
#
# 用法: bash install-dkp-deps.sh <ascii-pkg-dir>
#   <ascii-pkg-dir> 必须是纯 ASCII 路径 —— 预先下载好的 .pkg.tar.* 放在那里。

set -uo pipefail

PKGDIR="${1:-}"
if [ -z "$PKGDIR" ] || [ ! -d "$PKGDIR" ]; then
    echo "FATAL: pkg dir not usable: '$PKGDIR'"
    exit 2
fi

echo "=== env ==="
echo "DEVKITPRO=${DEVKITPRO:-<unset>}"
echo "MSYSTEM=${MSYSTEM:-<unset>}"
echo "PATH has pacman: $(command -v pacman || echo NO)"
echo "pkgdir: $PKGDIR"
ls -1 "$PKGDIR"

# ---------------------------------------------------------------------------
# 1) 先装预先下载好的 5 个包（ffmpeg / libmpv / libuam / nspmini / hacBrewPack）。
#    这几个不在 devkitPro 官方仓库里，必须用 -U 本地安装。
# ---------------------------------------------------------------------------
echo ""
echo "=== install local packages (-U) ==="
LOCAL_ARGS=()
for f in "$PKGDIR"/*.pkg.tar.zst "$PKGDIR"/*.pkg.tar.xz; do
    [ -f "$f" ] && LOCAL_ARGS+=("$f")
done

if [ ${#LOCAL_ARGS[@]} -eq 0 ]; then
    echo "no local packages found"
else
    pacman -U --noconfirm --needed "${LOCAL_ARGS[@]}" 2>&1 | tail -40
    echo "local-install-exit=${PIPESTATUS[0]}"
fi

# ---------------------------------------------------------------------------
# 2) 装仓库里的依赖（dav1d / libass / libfribidi / mbedtls / lua51 / mesa）。
#    libmpv 的 PKGBUILD 依赖 switch-liblua51，ffmpeg 依赖 switch-dav1d，
#    这两个是最容易漏掉的。
# ---------------------------------------------------------------------------
echo ""
echo "=== sync package db ==="
pacman -Sy --noconfirm 2>&1 | tail -8
echo "sync-exit=${PIPESTATUS[0]}"

echo ""
echo "=== install repo deps (-S) ==="
pacman -S --needed --noconfirm \
    dkp-toolchain-vars \
    switch-dav1d \
    switch-libass \
    switch-libfribidi \
    switch-mbedtls \
    switch-liblua51 \
    switch-mesa \
    switch-pkg-config \
    switch-zlib \
    switch-bzip2 \
    switch-freetype \
    switch-harfbuzz \
    2>&1 | tail -50
echo "deps-install-exit=${PIPESTATUS[0]}"

echo ""
echo "=== verify installed (switch-*) ==="
pacman -Q 2>/dev/null | grep -E '^(switch-|deko3d|libuam|hacBrewPack)' | sort

echo ""
echo "=== verify key headers/libs ==="
PREFIX="${DEVKITPRO:-/opt/devkitpro}/portlibs/switch"
for probe in \
    "$PREFIX/lib/libmpv.a" \
    "$PREFIX/lib/libavcodec.a" \
    "$PREFIX/lib/libavformat.a" \
    "$PREFIX/lib/libmbedtls.a" \
    "$PREFIX/lib/libdav1d.a" \
    "$PREFIX/lib/libass.a" \
    "$PREFIX/include/mpv/client.h" \
    "$PREFIX/include/dav1d/dav1d.h"
do
    if [ -e "$probe" ]; then echo "OK   $probe"; else echo "MISS $probe"; fi
done
