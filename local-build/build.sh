#!/bin/sh
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
OBS_VERSION=32.2.2
SIMDE_VERSION=v0.8.2
DEPS="$HERE/.deps"
OBS_LIBOBS_INCLUDE=${OBS_LIBOBS_INCLUDE:-"$DEPS/obs-studio-$OBS_VERSION/libobs"}
OBS_SIMDE_INCLUDE=${OBS_SIMDE_INCLUDE:-"$DEPS/simde"}
PLUGIN_DIR="$HOME/Library/Application Support/obs-studio/plugins"
BUNDLE="$HERE/build/gd-scene-tree.plugin"

if [ ! -f "$OBS_LIBOBS_INCLUDE/obs-module.h" ]; then
  mkdir -p "$DEPS"
  curl -sL -o "$DEPS/obs-$OBS_VERSION.tar.gz" \
    "https://github.com/obsproject/obs-studio/archive/refs/tags/$OBS_VERSION.tar.gz"
  tar xzf "$DEPS/obs-$OBS_VERSION.tar.gz" -C "$DEPS"
fi

if [ ! -f "$OBS_SIMDE_INCLUDE/simde/x86/sse2.h" ]; then
  mkdir -p "$DEPS"
  rm -rf "$OBS_SIMDE_INCLUDE"
  git clone --quiet --depth 1 --branch "$SIMDE_VERSION" \
    https://github.com/simd-everywhere/simde.git "$OBS_SIMDE_INCLUDE"
fi

cmake -S "$HERE" -B "$HERE/build" -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DOBS_LIBOBS_INCLUDE="$OBS_LIBOBS_INCLUDE" \
  -DOBS_SIMDE_INCLUDE="$OBS_SIMDE_INCLUDE"
cmake --build "$HERE/build"

BIN="$BUNDLE/Contents/MacOS/gd-scene-tree"
for dep in $(otool -L "$BIN" | awk '/\/Qt[A-Za-z]+\.framework\//{print $1}'); do
  name=$(basename "$dep")
  install_name_tool -change "$dep" "@rpath/$name.framework/Versions/A/$name" "$BIN"
done

codesign --force --sign - --options runtime --deep "$BUNDLE"

mkdir -p "$PLUGIN_DIR"
rm -rf "$PLUGIN_DIR/gd-scene-tree.plugin"
cp -R "$BUNDLE" "$PLUGIN_DIR/"
echo "Installed to $PLUGIN_DIR/gd-scene-tree.plugin"
