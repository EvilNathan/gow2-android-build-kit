package com.gow2.android;

import android.util.Log;
import android.view.Surface;

/**
 * Java side of the native host (libps2EntryRunner.so).  Every native symbol here is implemented in
 * android-runner/native/host/jni_bridge.cpp.
 *
 * <p>No exception ever escapes a native call; the exports guard with try/catch on the C++ side.
 */
public final class NativeBridge {

    private static final String TAG = "PS2X";
    private static final String LIBRARY = "ps2EntryRunner";

    /** Rumble sink implemented by GameActivity (vibrator). */
    public interface RumbleSink {
        void onRumble(float low, float high);
    }

    private static volatile RumbleSink rumbleSink;
    private static volatile boolean loaded;
    private static volatile String loadError = "";

    static {
        try {
            System.loadLibrary(LIBRARY);
            loaded = true;
        } catch (Throwable error) {
            // Keep the app usable (settings + ISO import still work) and surface the reason in the UI.
            loadError = String.valueOf(error.getMessage());
            Log.e(TAG, "cannot load " + LIBRARY, error);
        }
    }

    private NativeBridge() {
    }

    public static boolean isLoaded() {
        return loaded;
    }

    public static String loadError() {
        return loadError;
    }

    public static void setRumbleSink(RumbleSink sink) {
        rumbleSink = sink;
    }

    /** Called from the native host thread. */
    public static void dispatchRumble(float low, float high) {
        RumbleSink sink = rumbleSink;
        if (sink != null) {
            sink.onRumble(low, high);
        }
    }

    // --- lifecycle ---------------------------------------------------------
    public static native void nativeStart(String filesDir, String settingsJson);

    public static native void nativeStop();

    public static native void nativeSetSurface(Surface surface);

    public static native void nativeSurfaceChanged(int width, int height);

    public static native void nativeRequestExit();

    public static native void nativeSetPaused(boolean paused);

    // --- input -------------------------------------------------------------
    public static native void nativeSetPadState(int buttons, int lx, int ly, int rx, int ry);

    // --- diagnostics -------------------------------------------------------
    public static native String nativeLogPath();

    public static native String nativeHostStats();

    public static native void nativeCrashTest();

    // --- disc --------------------------------------------------------------
    /** Extracts the boot ELF from an ISO9660 image; returns a JSON object. */
    public static native String nativeExtractDisc(String isoPath, String filesDir);
}
