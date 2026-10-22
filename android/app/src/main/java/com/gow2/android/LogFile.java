package com.gow2.android;

import android.content.Context;
import android.content.pm.PackageInfo;
import android.os.Build;
import android.os.Environment;
import android.system.Os;
import android.util.DisplayMetrics;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.PrintWriter;
import java.io.StringWriter;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

/**
 * Session log shared by Java and the native engine.
 *
 * Prefers /sdcard/gow2/log.txt. On Android 11+ that folder needs "All files
 * access"; when it is missing the log falls back to the app-specific external
 * folder and the launcher offers a button to grant access.
 */
public final class LogFile {
    static final String FOLDER = "gow2";
    static final String NAME = "log.txt";
    private static File sDir;

    private LogFile() {
    }

    static synchronized File dir(Context context) {
        if (sDir != null) return sDir;
        File shared = new File(Environment.getExternalStorageDirectory(), FOLDER);
        try {
            if (!shared.isDirectory() && !shared.mkdirs()) throw new java.io.IOException("mkdirs failed");
            File probe = new File(shared, ".probe");
            try (FileOutputStream out = new FileOutputStream(probe)) {
                out.write('x');
            }
            //noinspection ResultOfMethodCallIgnored
            probe.delete();
            sDir = shared;
        } catch (Exception denied) {
            File fallback = new File(context.getExternalFilesDir(null), FOLDER);
            if (fallback.mkdirs() || fallback.isDirectory()) sDir = fallback;
        }
        return sDir;
    }

    static String path(Context context) {
        File dir = dir(context);
        return dir == null ? null : new File(dir, NAME).getAbsolutePath();
    }

    /** True when /sdcard/gow2 is the active location. */
    static boolean usingShared(Context context) {
        File dir = dir(context);
        if (dir == null) return false;
        return dir.getAbsolutePath().startsWith(Environment.getExternalStorageDirectory().getAbsolutePath());
    }

    /** Points the native engine at the log file and writes a session header. */
    static void prepare(Context context) {
        String path = path(context);
        if (path == null) return;
        try {
            Os.setenv("PS2_LOG_FILE", path, true);
        } catch (Exception ignored) {
        }

        StringBuilder header = new StringBuilder();
        header.append("\n=== session ");
        header.append(new SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US).format(new Date()));
        header.append(" ===\n");
        header.append("device: ").append(Build.MANUFACTURER).append(' ').append(Build.MODEL)
                .append(" | Android ").append(Build.VERSION.RELEASE)
                .append(" (SDK ").append(Build.VERSION.SDK_INT).append(") | ")
                .append(Build.SUPPORTED_ABIS.length > 0 ? Build.SUPPORTED_ABIS[0] : "?").append('\n');
        try {
            PackageInfo info = context.getPackageManager().getPackageInfo(context.getPackageName(), 0);
            long code = Build.VERSION.SDK_INT >= 28 ? info.getLongVersionCode() : info.versionCode;
            header.append("app: ").append(info.versionName).append(" (").append(code).append(")\n");
        } catch (Exception ignored) {
        }
        DisplayMetrics metrics = context.getResources().getDisplayMetrics();
        header.append("display: ").append(metrics.widthPixels).append('x').append(metrics.heightPixels)
                .append(" @ ").append(String.format(Locale.US, "%.2f", metrics.density)).append("x\n");
        header.append("log: ").append(path).append('\n');
        appendRaw(context, header.toString());
    }

    static synchronized void append(Context context, String message) {
        if (!message.endsWith("\n")) message += "\n";
        String stamp = new SimpleDateFormat("HH:mm:ss", Locale.US).format(new Date());
        appendRaw(context, "[" + stamp + "] " + message);
    }

    private static void appendRaw(Context context, String text) {
        File dir = dir(context);
        if (dir == null) return;
        try (FileOutputStream out = new FileOutputStream(new File(dir, NAME), true)) {
            out.write(text.getBytes(StandardCharsets.UTF_8));
            out.flush();
        } catch (Exception ignored) {
        }
    }

    /** Java-side crash traces land in the same file as the native breadcrumbs. */
    static void installHandler(final Context context) {
        final Thread.UncaughtExceptionHandler previous = Thread.getDefaultUncaughtExceptionHandler();
        Thread.setDefaultUncaughtExceptionHandler((thread, error) -> {
            StringWriter trace = new StringWriter();
            error.printStackTrace(new PrintWriter(trace));
            append(context, "[java] FATAL on " + thread.getName() + "\n" + trace);
            if (previous != null) previous.uncaughtException(thread, error);
        });
    }

    /** Last {@code maxBytes} of the log, decoded as UTF-8. */
    static String tail(Context context, int maxBytes) {
        File dir = dir(context);
        if (dir == null) return "";
        File file = new File(dir, NAME);
        try (FileInputStream in = new FileInputStream(file)) {
            long length = file.length();
            long skip = Math.max(0, length - maxBytes);
            long skipped = 0;
            while (skipped < skip) {
                long step = in.skip(skip - skipped);
                if (step <= 0) break;
                skipped += step;
            }
            byte[] data = new byte[(int) Math.min(maxBytes, length)];
            int read = 0;
            while (read < data.length) {
                int count = in.read(data, read, data.length - read);
                if (count < 0) break;
                read += count;
            }
            return new String(data, 0, read, StandardCharsets.UTF_8);
        } catch (Exception error) {
            return "Could not read " + file + ": " + error;
        }
    }
}
