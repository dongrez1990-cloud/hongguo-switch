#!/usr/bin/env bash
# DO NOT USE -- kept only as a record of a mistake.
#
# This script used to remove -lplacebo from mpv.pc on the theory that nothing
# referenced libplacebo. That theory came from
#     aarch64-none-elf-nm -u libmpv.a | grep -ci placebo
# which returned 0 -- a FALSE NEGATIVE, because libplacebo's symbols are
# prefixed `pl_` (pl_buf_create, pl_filter_bicubic, pl_color_space_srgb, ...)
# and the string "placebo" appears nowhere in them. The correct pattern is
# `\bpl_[a-z]`.
#
# Removing the flag made the link fail with hundreds of undefined pl_* symbols,
# proving libplacebo is a genuine dependency. switch-libmpv's mpv.pc is right to
# list it; the defect is in the repacked prebuilt, which omits the library.
#
# The real fix is to BUILD libplacebo, which is what scripts/build-libplacebo.sh
# does (it also restores -lplacebo in mpv.pc first, since the removal could not be
# reverted without elevation at the time).
#
# Anything this script once did is now wrong. It refuses to run.

set -euo pipefail

echo "Refusing to run: this script encodes a false-negative analysis and would" >&2
echo "break the static link by removing a real dependency (-lplacebo)." >&2
echo "" >&2
echo "Use scripts/build-libplacebo.sh instead." >&2
exit 1
