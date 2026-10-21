#!/bin/zsh
# Sets up a new Bobble over USB with the latest release (no ESP-IDF or build needed, just esptool):
#   ./flash_new.sh                       erase it, then flash (a new Bobble, or one changing hands)
#   ./flash_new.sh --keep                flash only, keeping its Wi-Fi, pairing and settings
#   ./flash_new.sh --port /dev/cu.usbmodem3101
# The release carries the four pieces a blank board needs (build_app.sh --release puts them there). After this the
# Mac app keeps it up to date over Wi-Fi. (In a browser instead: https://alexandermaat99.github.io/bobble/)
set -e -o pipefail
repo=alexandermaat99/bobble
keep=0 port=""
while [[ $# -gt 0 ]]; do
    case $1 in
        --keep) keep=1 ;;
        --port) port=$2; shift ;;
        *) echo "usage: $0 [--keep] [--port /dev/cu.usbmodem…]"; exit 1 ;;
    esac
    shift
done

# esptool: on its own, or the Python module (pip install esptool), or ESP-IDF's
if command -v esptool.py >/dev/null; then esptool=(esptool.py)
elif command -v esptool >/dev/null; then esptool=(esptool)
elif python3 -m esptool version >/dev/null 2>&1; then esptool=(python3 -m esptool)
else
    echo "This needs esptool. Install it with:  python3 -m pip install --user esptool  (or: brew install esptool)"
    exit 1
fi

if [[ -z $port ]]; then
    ports=(/dev/cu.usbmodem*(N))
    [[ ${#ports} -gt 0 ]] || { echo "No Bobble found: plug it in with a USB-C cable that carries data."; exit 1; }
    [[ ${#ports} -eq 1 ]] || { echo "Several USB devices: pick one with --port (${ports[*]})"; exit 1; }
    port=${ports[1]}
fi

dir=$(mktemp -d)
url=https://github.com/$repo/releases/latest/download
echo "Downloading the latest Bobble release..."
for f in bobble-bootloader.bin bobble-partitions.bin bobble-otadata.bin bobble-firmware.bin; do
    curl -fsSL -o $dir/$f $url/$f || { echo "Couldn't download $f (is the latest release missing it?)"; exit 1; }
done

if (( ! keep )); then
    echo "Erasing the Bobble on $port..."
    $esptool --chip esp32s3 --port $port erase_flash
fi
echo "Flashing..."
$esptool --chip esp32s3 --port $port -b 460800 --before default_reset --after hard_reset write_flash \
    --flash_mode dio --flash_freq 80m --flash_size 16MB \
    0x0 $dir/bobble-bootloader.bin 0x8000 $dir/bobble-partitions.bin 0xf000 $dir/bobble-otadata.bin 0x20000 $dir/bobble-firmware.bin
rm -rf $dir
echo
echo "Done: the Bobble restarts by itself."
(( keep )) || echo "On its screen: tap the bubble to set up Wi-Fi, then pair it from the Bobble app on your Mac."
