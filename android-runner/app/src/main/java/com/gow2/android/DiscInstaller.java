package com.gow2.android;

import android.content.ContentResolver;
import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.os.StatFs;
import android.provider.OpenableColumns;
import android.util.Log;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

/** Copies a SAF-picked disc image into the app files dir so the engine can open it by path. */
public final class DiscInstaller {

    private static final String TAG = "PS2X";
    private static final int BUFFER_SIZE = 64 * 1024;
    /** Keep this much free space after a copy so the app (logs, ELF, saves) still fits. */
    private static final long FREE_SPACE_MARGIN = 64L * 1024 * 1024;

    public interface Listener {
        /** Called from the worker thread; {@code total} is -1 when the size is unknown. */
        void onProgress(long copied, long total);
    }

    public static final class Result {
        public final boolean ok;
        public final boolean skipped;
        public final File file;
        public final String error;

        private Result(boolean ok, boolean skipped, File file, String error) {
            this.ok = ok;
            this.skipped = skipped;
            this.file = file;
            this.error = error;
        }

        static Result success(File file, boolean skipped) {
            return new Result(true, skipped, file, "");
        }

        static Result failure(String error) {
            return new Result(false, false, null, error == null ? "unknown error" : error);
        }
    }

    private DiscInstaller() {
    }

    /**
     * Copies {@code source} to {@code destination}.  Skips the copy when the destination already has
     * the same size, and checks free space first (a PS2 DVD image is several gigabytes).
     */
    public static Result install(Context context, Uri source, File destination, Listener listener) {
        if (source == null) {
            return Result.failure("ISO belum dipilih");
        }
        ContentResolver resolver = context.getContentResolver();
        long sourceSize = querySize(resolver, source);

        if (destination.isFile() && sourceSize > 0 && destination.length() == sourceSize) {
            return Result.success(destination, true);
        }

        File parent = destination.getParentFile();
        if (parent != null && !parent.isDirectory() && !parent.mkdirs()) {
            return Result.failure("Folder data tidak bisa dibuat: " + parent);
        }

        if (sourceSize > 0) {
            try {
                StatFs stat = new StatFs(parent != null ? parent.getPath() : destination.getPath());
                long free = stat.getAvailableBytes();
                if (free < sourceSize + FREE_SPACE_MARGIN) {
                    return Result.failure(String.format(java.util.Locale.US,
                            "Ruang penyimpanan tidak cukup: butuh %.2f GB, tersedia %.2f GB",
                            (sourceSize + FREE_SPACE_MARGIN) / 1073741824.0, free / 1073741824.0));
                }
            } catch (RuntimeException error) {
                Log.w(TAG, "free space check failed", error);
            }
        }

        File temporary = new File(destination.getPath() + ".part");
        try (InputStream input = resolver.openInputStream(source);
             OutputStream output = new FileOutputStream(temporary)) {
            if (input == null) {
                return Result.failure("ISO tidak bisa dibuka (izin akses dicabut?)");
            }
            byte[] buffer = new byte[BUFFER_SIZE];
            long copied = 0;
            long lastReport = 0;
            int read;
            while ((read = input.read(buffer)) > 0) {
                output.write(buffer, 0, read);
                copied += read;
                if (listener != null && copied - lastReport >= 32L * 1024 * 1024) {
                    lastReport = copied;
                    listener.onProgress(copied, sourceSize);
                }
                if (Thread.currentThread().isInterrupted()) {
                    return Result.failure("Impor dibatalkan");
                }
            }
            output.flush();
            if (output instanceof FileOutputStream) {
                ((FileOutputStream) output).getFD().sync();
            }
            if (listener != null) {
                listener.onProgress(copied, sourceSize > 0 ? sourceSize : copied);
            }
        } catch (SecurityException error) {
            Log.w(TAG, "no permission for " + source, error);
            return Result.failure("Izin baca ISO dicabut; pilih ulang berkasnya");
        } catch (IOException error) {
            Log.w(TAG, "copy failed", error);
            return Result.failure("Gagal menyalin ISO: " + error.getMessage());
        }

        // Replace the destination atomically so a half-written image is never visible.
        if (destination.exists() && !destination.delete()) {
            Log.w(TAG, "cannot remove old " + destination);
        }
        if (!temporary.renameTo(destination)) {
            return Result.failure("Gagal menyimpan gow2.iso di " + destination.getAbsolutePath());
        }
        return Result.success(destination, false);
    }

    /** Reads the size of a SAF document, or -1 when the provider does not expose it. */
    public static long querySize(ContentResolver resolver, Uri uri) {
        try (Cursor cursor = resolver.query(uri, new String[]{OpenableColumns.SIZE}, null, null, null)) {
            if (cursor != null && cursor.moveToFirst()) {
                int column = cursor.getColumnIndex(OpenableColumns.SIZE);
                if (column >= 0 && !cursor.isNull(column)) {
                    return cursor.getLong(column);
                }
            }
        } catch (RuntimeException error) {
            Log.w(TAG, "size query failed", error);
        }
        return -1;
    }

    /** Human readable byte count for the launcher UI. */
    public static String formatBytes(long bytes) {
        if (bytes < 0) {
            return "?";
        }
        if (bytes >= 1073741824L) {
            return String.format(java.util.Locale.US, "%.2f GB", bytes / 1073741824.0);
        }
        if (bytes >= 1048576L) {
            return String.format(java.util.Locale.US, "%.1f MB", bytes / 1048576.0);
        }
        return String.format(java.util.Locale.US, "%.0f KB", bytes / 1024.0);
    }

    /** Debug helper: tests a file exists and is readable without opening the whole image. */
    public static boolean isReadable(File file) {
        if (file == null || !file.isFile() || file.length() <= 0) {
            return false;
        }
        try (FileInputStream input = new FileInputStream(file)) {
            return input.read() >= 0;
        } catch (IOException error) {
            return false;
        }
    }
}
