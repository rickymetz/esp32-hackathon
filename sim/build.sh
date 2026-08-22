#!/usr/bin/env bash
# Configure and build the simulator. Re-run any time after editing sources.
set -euo pipefail

cd "$(dirname "$0")"

if [ ! -d external/lvgl ] || [ ! -d external/lua ]; then
    echo "Third-party sources missing -- running setup.sh first."
    ./setup.sh
fi

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build sim

echo
echo "Built sim/build/sim . Try:"
echo "  ./sim/build/sim --sdroot . run apps/counter.lua : tap 184 224 : shot out.png"
