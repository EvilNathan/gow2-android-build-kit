package com.gow2.android;

import android.content.Context;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;

/**
 * Thread-safe settings persistence in {@code getExternalFilesDir(null)/settings.json}.
 *
 * <p>The external files dir is app-private and needs no storage permission; when it is unavailable
 * (rare) the internal files dir is used instead.
 */
public final class SettingsStore {

    private static final String TAG = "PS2X";
    private static final String SETTINGS_FILE = "settings.json";

    /** Boot ELF extracted from the disc; the CMake default boot path points at this name. */
    public static final String BOOT_ELF_NAME = "SCUS_974.81";
    /** Disc image the engine opens (GOW2_ISO); also the sibling fallback name. */
    public static final String ISO_NAME = "gow2.iso";

    private final File file;
    private Settings cached;

    public SettingsStore(Context context) {
        this.file = new File(filesDir(context), SETTINGS_FILE);
    }

    /** App files directory shared with the native host; never null. */
    public static File filesDir(Context context) {
        File external = context.getExternalFilesDir(null);
        if (external != null) {
            return external;
        }
        return context.getFilesDir();
    }

    public static File isoFile(Context context) {
        return new File(filesDir(context), ISO_NAME);
    }

    public static File bootElfFile(Context context) {
        return new File(filesDir(context), BOOT_ELF_NAME);
    }

    public static String filesPath(Context context) {
        return filesDir(context).getAbsolutePath();
    }

    public File file() {
        return file;
    }

    /** Current settings; returns a copy so callers can mutate freely. */
    public synchronized Settings get() {
        if (cached == null) {
            cached = read();
        }
        return cached.copy();
    }

    public synchronized void save(Settings settings) {
        if (settings == null) {
            return;
        }
        settings.sanitize();
        cached = settings.copy();
        write(settings.toJson());
    }

    private Settings read() {
        if (!file.isFile()) {
            return new Settings();
        }
        try (RandomAccessFile input = new RandomAccessFile(file, "r")) {
            byte[] buffer = new byte[(int) Math.min(input.length(), 64 * 1024)];
            input.readFully(buffer);
            return Settings.fromJson(new String(buffer, StandardCharsets.UTF_8));
        } catch (IOException | RuntimeException error) {
            Log.w(TAG, "settings.json unreadable, using defaults", error);
            return new Settings();
        }
    }

    private void write(String json) {
        File parent = file.getParentFile();
        if (parent != null && !parent.isDirectory() && !parent.mkdirs()) {
            Log.w(TAG, "cannot create " + parent);
            return;
        }
        // Write to a temporary file first so a crash mid-write never truncates the settings.
        File temporary = new File(file.getPath() + ".tmp");
        try (FileOutputStream output = new FileOutputStream(temporary)) {
            output.write(json.getBytes(StandardCharsets.UTF_8));
            output.flush();
            output.getFD().sync();
        } catch (IOException error) {
            Log.w(TAG, "cannot write settings", error);
            return;
        }
        if (!temporary.renameTo(file)) {
            // renameTo can fail on some volumes; fall back to a direct overwrite.
            try (FileOutputStream output = new FileOutputStream(file)) {
                output.write(json.getBytes(StandardCharsets.UTF_8));
            } catch (IOException error) {
                Log.w(TAG, "cannot replace settings", error);
            }
        }
    }
}
