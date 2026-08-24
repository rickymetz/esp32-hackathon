# Prebuilt launcher firmware

You do **not** need ESP-IDF to use this.

```bash
./flash.sh                       # auto-detects the board
./flash.sh /dev/cu.usbmodem101   # or name the port
```

On first run this downloads the latest release and caches it, then flashes.
Pin a specific one with `LAUNCHER_VERSION=v0.1.0 ./flash.sh`.

Then put your `.lua` apps in `/apps` on the microSD card.

## Where the binaries live

**Not in this repo.** They are attached to a [GitHub
release](https://github.com/rickymetz/esp32-hackathon/releases) as
`esp32-launcher-<version>.zip`. Four binaries and a 3.7 MB archive do not
belong in git history, and a committed copy silently rots — the old
`firmware/bin/` ended up 171 commits behind the source it claimed to be.

The archive contains:

| | |
| --- | --- |
| `bin/` | the four images, plus `OFFSETS` and `FLASH_ARGS` |
| `flash.sh` | the same script as here; works standalone from the extracted folder |
| `MANIFEST` | version, commit, build time, IDF version, and whether the tree was dirty |
| `SHA256SUMS` | checksums for every image |

**All four images are required.** The previous hand-maintained `firmware/bin/`
shipped only three — it was missing `srmodels.bin`, the 2.76 MB speech model
blob at `0x810000`, so every board flashed from it had `voice.available()`
return false with no indication why. `flash.sh` now reads the file list and
offsets from `bin/OFFSETS`, which `package_firmware.sh` generates from the
build's own `flasher_args.json`, so that class of drift cannot recur.

## Cutting a release

**CI does it.** Merge to `main`, then tag that commit:

```bash
git tag v0.1.0 && git push origin v0.1.0
```

`.github/workflows/release.yml` builds against ESP-IDF v5.5.5, runs
`tools/package_firmware.sh`, and publishes the archive as a release. Because
CI builds from a clean checkout of the tag, `MANIFEST` records the real
commit with `dirty=no` — which is the point. A package built on a laptop can
quietly contain uncommitted work, and a binary that no commit reproduces is
how `firmware/bin/` drifted 171 commits from its source in the first place.

To exercise the pipeline without publishing anything, run the workflow
manually (`workflow_dispatch`): it builds and attaches the archive as a
workflow artifact and creates no release.

Building a package locally still works and is the right thing for testing a
board before you tag:

```bash
cd launcher && idf.py build && cd ..
tools/package_firmware.sh v0.0.0-local
cd dist/esp32-launcher-v0.0.0-local && ./flash.sh
```

`MANIFEST` will say `dirty=yes` if your tree had uncommitted changes. That
is fine locally and is exactly what should never reach a release.

## If flashing fails

The board uses the S3's native USB, so a firmware crash takes the port with
it and no software reset recovers it. Recovery is physical:

1. Hold PWR for at least 6 seconds to power off.
2. Hold BOOT down and keep holding.
3. Press PWR to power on, then release BOOT.
4. Flash again.
5. Power-cycle afterwards — it does not leave download mode on its own.

Also check nothing else is holding the port; a serial monitor blocks it.
