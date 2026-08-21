# Prebuilt launcher firmware

You do **not** need ESP-IDF to use this.

    ./flash.sh                 # auto-detects the board
    ./flash.sh /dev/cu.usbmodem101   # or name the port

Then put your `.lua` apps in `/apps` on the microSD card.

These binaries are refreshed by hand when the launcher changes. If yours looks
out of date, ask, or build from `launcher/`.
