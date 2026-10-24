package com.headsplosion.lostworldvr;

import org.libsdl.app.SDL;
import org.libsdl.app.SDLActivity;

/** SDL's activity, pointed at libmain.so (the emulator). All the work happens in native code. */
public class SupermodelActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "main" };
    }

    @Override
    protected void loadLibraries() {
        // SDL's Java HID code calls into libhidapi.so when SDL was built with HIDAPI. Load it if it
        // exists; a build without it is fine, since the Quest controllers come in through OpenXR.
        try {
            SDL.loadLibrary("hidapi");
        } catch (Throwable ignored) {
        }
        super.loadLibraries();
    }
}
