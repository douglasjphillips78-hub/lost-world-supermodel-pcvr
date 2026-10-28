package com.headsplosion.lostworldvr;

import android.Manifest;
import android.content.pm.PackageManager;
import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
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

    /** Called from native code (not on the main thread): POST one line of text to the room directory, return its reply. */
    public String dirRequest(String url, String line, int timeoutMs) {
        HttpURLConnection c = null;
        try {
            c = (HttpURLConnection) new URL(url).openConnection();
            c.setConnectTimeout(timeoutMs);
            c.setReadTimeout(timeoutMs);
            c.setRequestMethod("POST");
            c.setDoOutput(true);
            c.setRequestProperty("Content-Type", "text/plain");
            byte[] body = line.getBytes("UTF-8");
            try (OutputStream o = c.getOutputStream()) { o.write(body); }
            if (c.getResponseCode() != 200) return null;
            ByteArrayOutputStream bo = new ByteArrayOutputStream();
            try (InputStream in = c.getInputStream()) {
                byte[] buf = new byte[1024];
                int n;
                while ((n = in.read(buf)) > 0 && bo.size() < 8192) bo.write(buf, 0, n);
            }
            return bo.toString("UTF-8");
        } catch (Exception e) {
            return null;
        } finally {
            if (c != null) c.disconnect();
        }
    }
}
