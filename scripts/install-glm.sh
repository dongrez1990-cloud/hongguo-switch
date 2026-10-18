#!/usr/bin/env bash
# Install GLM (header-only) and re-run the wiliwili compile probe.
#
# borealis's deko3d nanovg renderer includes <glm/vec2.hpp>. GLM is not one of
# wiliwili's git submodules, it is not vendored in library/, and the devkitPro
# switch-glm package is unreachable (403), so clone upstream GLM and drop its
# headers into the portlibs prefix.
#
# Usage: install-glm.sh <ascii-root>

set -uo pipefail

ROOT="${1:?ascii root required}"
SRC="$ROOT/third_party/src"
LOGDIR="$ROOT/build-logs"
mkdir -p "$LOGDIR"

export DEVKITPRO=/opt/devkitpro
export PORTLIBS_PREFIX="$DEVKITPRO/portlibs/switch"
export PATH="$DEVKITPRO/tools/bin:$DEVKITPRO/devkitA64/bin:$PORTLIBS_PREFIX/bin:$PATH"

echo "=================== install GLM ==================="
(
  set -e
  cd "$SRC"
  if [ ! -d glm/.git ]; then
    rm -rf glm
    git -c http.sslBackend=openssl clone --depth 1 --quiet https://github.com/g-truc/glm.git glm
  fi
  cd glm
  # Pin to a stable tag rather than tracking master, so the build is reproducible.
  git -c http.sslBackend=openssl fetch --depth 1 --quiet origin tag 1.0.1 2>/dev/null || true
  git checkout --quiet 1.0.1 2>/dev/null || echo "note: staying on default branch"
  echo "glm version: $(git describe --tags --always 2>/dev/null)"

  # GLM is header-only; the public headers live under glm/glm/.
  mkdir -p "$PORTLIBS_PREFIX/include"
  rm -rf "$PORTLIBS_PREFIX/include/glm"
  cp -r glm "$PORTLIBS_PREFIX/include/glm"
  echo "installed headers: $(find "$PORTLIBS_PREFIX/include/glm" -name '*.hpp' | wc -l)"
  ls "$PORTLIBS_PREFIX/include/glm" | head
) > "$LOGDIR/install-glm.log" 2>&1
if [ -f "$PORTLIBS_PREFIX/include/glm/vec2.hpp" ]; then
  echo ">>> OK glm installed"
  grep -E 'glm version|installed headers' "$LOGDIR/install-glm.log"
else
  echo ">>> FAIL glm"; tail -20 "$LOGDIR/install-glm.log"
fi

echo ""
echo "=================== re-probe wiliwili ==================="
bash "$ROOT/scripts/probe-wiliwili-compile.sh" "$ROOT"
