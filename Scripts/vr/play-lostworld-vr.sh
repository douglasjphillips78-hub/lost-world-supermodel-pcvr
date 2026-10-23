#!/bin/sh
# The Lost World PC VR - Linux. Start SteamVR (or Monado, WiVRn, ALVR) first and make it the
# active OpenXR runtime; then run this script.
cd "$(dirname "$0")" || exit 1
mkdir -p Saves NVRAM
ROM=ROMs/lostwsga.zip
[ -f "$ROM" ] || ROM=../lostwsga.zip
if [ ! -f "$ROM" ]; then
  echo "lostwsga.zip not found. Put it in the ROMs folder next to this script."
  exit 1
fi
# Bundled libraries (SDL2, SDL2_net, GLU, the Khronos OpenXR loader) are used when the system
# has none of its own.
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:+$LD_LIBRARY_PATH:}$(pwd)/lib"
# The VR mode shares an OpenGL (GLX) context with the OpenXR runtime: run under X11 / XWayland.
export SDL_VIDEODRIVER=x11
./supermodel "$ROM" -vr -window -res=992,768 -no-wide-screen "$@"
status=$?
[ $status -ne 0 ] && echo "Supermodel stopped with an error - see Supermodel.log in this folder."
exit $status
