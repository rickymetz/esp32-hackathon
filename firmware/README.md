# Prebuilt launcher firmware

You do **not** need ESP-IDF to use this.

    ./flash.sh                 # auto-detects the board
    ./flash.sh /dev/cu.usbmodem101   # or name the port

Then put your `.lua` apps in `/apps` on the microSD card.

These binaries are refreshed by hand when the launcher changes. If yours looks
out of date, ask, or build from `launcher/`.

`firmware/bin/BUILD_INFO` records which source commit the binaries were built
from — a git short SHA and the date they were staged. `flash.sh` prints it
before every flash, so you can see at a glance which revision is about to go
on the board. To check whether it's current, compare it against the tip of
your checkout:

    git log -1 --format=%h

If the SHAs differ, the binaries may predate later launcher changes — refresh
them from `launcher/` or ask whoever last staged them.
