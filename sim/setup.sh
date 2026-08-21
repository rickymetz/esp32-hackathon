#!/usr/bin/env bash
# Fetch the third-party sources the simulator builds against.
#
# LVGL and the Lua core are pinned to the versions the launcher uses (LVGL
# 9.5, Lua 5.5.0) and cloned into sim/external/, which is gitignored -- run
# this once after checkout, then build with ./build.sh.
set -euo pipefail

cd "$(dirname "$0")"
mkdir -p external

if [ ! -d external/lvgl ]; then
    echo "Cloning LVGL 9.5 ..."
    git clone --depth 1 --branch release/v9.5 https://github.com/lvgl/lvgl.git external/lvgl
else
    echo "external/lvgl already present, skipping"
fi

if [ ! -d external/lua ]; then
    echo "Cloning Lua 5.5.0 ..."
    git clone --depth 1 --branch v5.5.0 https://github.com/lua/lua.git external/lua
else
    echo "external/lua already present, skipping"
fi

echo "Done. Now run ./build.sh"
