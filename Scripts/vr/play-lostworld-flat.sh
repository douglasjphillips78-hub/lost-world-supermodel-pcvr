#!/bin/sh
# The Lost World in a flat window (no headset) - Linux.
cd "$(dirname "$0")" || exit 1
mkdir -p Saves NVRAM
ROM=ROMs/lostwsga.zip
[ -f "$ROM" ] || ROM=../lostwsga.zip
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:+$LD_LIBRARY_PATH:}$(pwd)/lib"
./supermodel "$ROM" -window -res=992,768 -crosshairs=1 "$@"
