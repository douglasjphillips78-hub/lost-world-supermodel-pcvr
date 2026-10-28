package com.headsplosion.lostworldvr;

import android.Manifest;
import android.content.pm.PackageManager;
import org.libsdl.app.SDL;
import org.libsdl.app.SDLActivity;

/** SDL's activity, pointed at libmain.so (the emulator). All the work happens in native code. */
public class SupermodelActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "main" };
    }

    @Override
    public void loadLibraries() {
        // SDL's Java HID code calls into libhidapi.so when SDL was built with HIDAPI. Load it if it
        // exists; a build without it is fine, since the Quest controllers come in through OpenXR.
        try {
            SDL.loadLibrary("hidapi");
        } catch (Throwable ignored) {
        }
        super.loadLibraries();
    }

    /** Called from native code: is the microphone allowed? */
    public boolean hasMicPermission() {
        return checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED;
    }

    /** Called from native code: show the system's microphone permission question. */
    public void requestMicPermission() {
        runOnUiThread(() -> requestPermissions(new String[] { Manifest.permission.RECORD_AUDIO }, 4242));
    }
}
