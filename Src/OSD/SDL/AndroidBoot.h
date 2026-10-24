#ifndef INCLUDED_ANDROIDBOOT_H
#define INCLUDED_ANDROIDBOOT_H

// Start-up glue for the Meta Quest (Android) build. Supermodel is a command-line program that
// expects a working directory holding Config/, ROMs/, NVRAM/, Saves/ and Assets/. On Android there
// is no command line and no obvious working directory, so this module supplies both.
#ifdef __ANDROID__

#include <string>
#include <vector>

namespace AndroidBoot
{
// chdir()s into the app's external files folder (visible over USB / adb as
// /sdcard/Android/data/<package>/files), creates the folders Supermodel needs, and copies the
// bundled Config/ and Assets/ out of the APK the first time (Games.xml and Music.xml are refreshed
// when the bundled copy changes; Supermodel.ini is never overwritten, so settings survive updates).
// Fills `args` with the argv the headset build runs with. Returns false, with a message already
// shown to the person wearing the headset, when the ROM is missing.
bool Prepare(std::vector<std::string>& args);
}

#endif  // __ANDROID__
#endif  // INCLUDED_ANDROIDBOOT_H
