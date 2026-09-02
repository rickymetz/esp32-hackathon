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

# Build the unit tests too. They used to be left out, so `sim/build/sim_tests`
# could sit stale for as long as you liked while reporting "all unit tests
# passed" -- which is worse than not running them, and did mislead a session
# into believing a new test was covering something it had never been compiled
# against. Building is cheap; a lying test binary is not.
ninja -C build sim_tests

echo
echo "Built sim/build/sim . Try:"
echo "  ./sim/build/sim --sdroot . run apps/counter.lua : tap 184 224 : shot out.png"
