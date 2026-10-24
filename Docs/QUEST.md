# Supermodel VR on Meta Quest (standalone APK)

This is a port of the PC VR build to run **on the headset itself**, with no PC or streaming. It is
experimental: nobody has measured whether a Quest can emulate a Sega Model 3 at a steady 60 fps while
drawing two eyes, so treat the first builds as a performance test.

## Install

1. Get `LostWorldVR-quest.apk` from the Releases page, or from the artifacts of the latest successful
   **Quest APK (Android)** run under Actions.
2. Sideload it: enable developer mode on the headset, connect over USB, then
   `adb install -r LostWorldVR-quest.apk` (or use Meta Quest Developer Hub / SideQuest).
3. Copy your own ROM set to the headset, for example
   `adb push swtrilgy.zip /sdcard/Android/data/com.headsplosion.lostworldvr/files/ROMs/`
   (start the app once first, so the folder exists; it also shows you the path if no ROM is found).
   Supported: `lostwsga.zip` (The Lost World), `swtrilgy.zip` (Star Wars Trilogy Arcade), and the clone
   sets `swtrilgya.zip` / `swtrilgyp.zip` (these also need `swtrilgy.zip`), `lostwsgp.zip`.
4. Launch **Supermodel VR** from *Unknown Sources* in the headset library.

With several ROMs installed, the first one found is started (Lost World, then Star Wars). To choose, create
`.../files/Config/StartGame.txt` containing the ROM name, e.g. `swtrilgy`.

Settings, NVRAM and saves live next to the ROM in `.../files/Config`, `NVRAM` and `Saves`. Optional gun
models go in `.../files/Assets/` (see `Assets/VR-MENU.txt`). The controls and options menu are the same
as the PC version (see the README).

## Star Wars Trilogy (flight stick)

Games that use an analog joystick and no light guns (Star Wars Trilogy) switch the controllers to flight
mode automatically:

- Thumbsticks (either hand) steer the stick. If up/down feels backwards, set `VRStickInvertY = 1`
  in `Config/Supermodel.ini` (it is the first thing to try if the ship dives when you push to climb).
- Triggers fire (the stick trigger). Grips are the stick's second button (the "event" button: missiles,
  bombs and similar).
- A / Y start, B / X coin, and the left-hand menu works as in the PC build.
- `VRStickDeadZone` (default 0.12) sets the dead zone around the centre.

The camera moves a lot in this game. If it makes you queasy, turn on cinema mode in the left menu's
Comfort tab so the picture sits on a fixed screen. Force feedback is not supported.

## Debugging

The emulator's console output is forwarded to logcat: `adb logcat -s SupermodelVR`. The VR statistics
line is printed there too (GPU time is not available on Quest and shows 0).

## What differs from the PC build

| Area | PC | Quest |
|---|---|---|
| Graphics API | OpenGL 4.1 core (GLEW) | OpenGL ES 3.2 through `Android/glcompat/GL/glew.h` |
| Shaders | `#version 410 core` | rewritten at load to `#version 320 es` (`GLESCompat::ConvertShader`) |
| 3D renderer | New3D or Legacy3D | New3D, triangle path only (quad rendering needs `noperspective`, which ES lacks) |
| OpenXR | `XR_KHR_opengl_enable`, runtime from the PC | `XR_KHR_opengl_es_enable` + `XR_KHR_android_create_instance`, loader from the APK |
| Start-up | command line | `AndroidBoot`: working folder, bundled config, fixed VR arguments |

## Building

CI does everything (`.github/workflows/quest-android.yml`). Locally you need JDK 17, the Android SDK with
NDK 26.3.11579264 and CMake 3.22.1, Gradle 8.7, and SDL2 2.30.9 / SDL2_net 2.2.0 unpacked into
`Android/third_party/SDL2` and `Android/third_party/SDL2_net`. Then:

    gradle -p Android :app:assembleRelease

The APK is `Android/app/build/outputs/apk/release/app-release.apk` (signed with the debug key, which is
all sideloading needs).
