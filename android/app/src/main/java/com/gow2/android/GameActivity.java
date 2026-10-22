package com.gow2.android;

import android.content.Intent;
import android.content.SharedPreferences;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;
import android.system.Os;
import android.widget.RelativeLayout;
import android.widget.Toast;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.util.ArrayList;

public final class GameActivity extends SDLActivity {
    private static ParcelFileDescriptor sIso;
    private static String sIsoUri;
    private static long sIsoSize;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        if (!openIso()) {
            // Still run the normal Activity path: Android requires onCreate to
            // reach super.onCreate before the activity may finish.
            super.onCreate(savedInstanceState);
            Toast.makeText(this, "No readable ISO. Open the launcher and select the game.", Toast.LENGTH_LONG).show();
            finish();
            return;
        }
        applyGpuPreferences();
        SharedPreferences prefs = getSharedPreferences(LauncherActivity.PREFS, MODE_PRIVATE);
        int scale = prefs.getInt(LauncherActivity.KEY_SCALE, 1);
        boolean safe = prefs.getBoolean(LauncherActivity.KEY_SAFE, false);
        boolean vuAot = prefs.getBoolean(LauncherActivity.KEY_VU_AOT, true);
        StringBuilder vuSkip = new StringBuilder();
        for (String hash : LauncherActivity.VU_NEW) {
            if (!prefs.getBoolean(LauncherActivity.KEY_VU_NEW + hash, false)) {
                if (vuSkip.length() > 0) vuSkip.append(',');
                vuSkip.append(hash);
            }
        }
        try {
            // Read by the WebGPU renderer when it creates its device.
            Os.setenv("PS2_SCALE", String.valueOf(scale == 2 ? 2 : 1), true);
            // Safe mode drops every optional GPU path (custom driver, turbo, DRS,
            // half-rate cadence) so a device that crashed on 2.0 still boots.
            Os.setenv("PS2_SAFE", safe ? "1" : "0", true);
            // Swapchain-only dynamic resolution; opt-in, the PS2 framebuffer stays 1x/2x.
            Os.setenv("PS2_DRS", !safe && prefs.getBoolean(LauncherActivity.KEY_DRS, false) ? "1" : "0", true);
            // Optional half-rate cadence for phones that cannot hold 60 fps.
            Os.setenv("PS2_FPS_CADENCE",
                    !safe && prefs.getBoolean(LauncherActivity.KEY_FPS30, false) ? "30" : "60", true);
            // VU1 AOT controls: the five programs added in 2.0 stay on the
            // interpreter unless the user enables them one by one.
            Os.setenv("PS2_VU_AOT", vuAot ? "1" : "0", true);
            Os.setenv("PS2_VU_SKIP", vuAot ? vuSkip.toString() : "", true);
        } catch (Exception ignored) {
        }
        LogFile.append(this, "[app] game start: iso=" + sIsoSize + " bytes, scale=" + scale
                + ", safe=" + safe
                + ", drs=" + prefs.getBoolean(LauncherActivity.KEY_DRS, false)
                + ", fps30=" + prefs.getBoolean(LauncherActivity.KEY_FPS30, false)
                + ", vu_aot=" + vuAot
                + ", vu_skip=" + (vuAot ? vuSkip : "all")
                + ", custom_driver=" + (prefs.getString(LauncherActivity.KEY_DRIVER_PATH, null) != null));
        super.onCreate(savedInstanceState);
        float alpha = getSharedPreferences(LauncherActivity.PREFS, MODE_PRIVATE)
                .getFloat(LauncherActivity.KEY_TOUCH_ALPHA, 0.86f);
        TouchOverlayView overlay = new TouchOverlayView(this);
        overlay.setAlpha(alpha);
        mLayout.addView(overlay, new RelativeLayout.LayoutParams(
                RelativeLayout.LayoutParams.MATCH_PARENT,
                RelativeLayout.LayoutParams.MATCH_PARENT));
    }

    @Override
    protected void onDestroy() {
        LogFile.append(this, "[app] game activity destroyed");
        super.onDestroy();
    }

    private void applyGpuPreferences() {
        try {
            SharedPreferences prefs = getSharedPreferences(LauncherActivity.PREFS, MODE_PRIVATE);
            if (prefs.getBoolean(LauncherActivity.KEY_SAFE, false)) return; // safe mode: stock driver, no turbo
            if (!LauncherActivity.isAdreno()) return; // Mali and other vendors keep the stock driver
            String driver = prefs.getString(LauncherActivity.KEY_DRIVER_PATH, null);
            if (driver != null) {
                File file = new File(driver);
                if (file.isFile()) {
                    File tmp = new File(getFilesDir(), "gpu-drivers/tmp");
                    tmp.mkdirs();
                    Os.setenv("PS2_VK_DRIVER", file.getAbsolutePath(), true);
                    Os.setenv("PS2_VK_HOOK_DIR", getApplicationInfo().nativeLibraryDir, true);
                    Os.setenv("PS2_VK_TMP", tmp.getAbsolutePath(), true);
                }
            }
            if (prefs.getBoolean(LauncherActivity.KEY_TURBO, false)) {
                Os.setenv("PS2_TURBO", "1", true);
            }
        } catch (Exception ignored) {
            // A missing or unreadable driver simply leaves the system driver in place.
        }
    }

    private boolean openIso() {
        String value = getSharedPreferences(LauncherActivity.PREFS, MODE_PRIVATE)
                .getString(LauncherActivity.KEY_ISO_URI, null);
        if (value == null) return false;
        if (sIso != null && value.equals(sIsoUri)) return true;
        if (sIso != null) {
            try {
                sIso.close();
            } catch (Exception ignored) {
            }
            sIso = null;
            sIsoUri = null;
        }
        Uri uri = Uri.parse(value);
        try {
            ParcelFileDescriptor fd = getContentResolver().openFileDescriptor(uri, "r");
            if (fd == null) return false;
            long size = fd.getStatSize();
            if (size <= 0) size = querySize(uri);
            if (size <= 0) {
                fd.close();
                return false;
            }
            sIso = fd;
            sIsoSize = size;
            sIsoUri = value;
            return true;
        } catch (Exception e) {
            return false;
        }
    }

    private long querySize(Uri uri) {
        try (Cursor cursor = getContentResolver().query(uri, new String[]{OpenableColumns.SIZE}, null, null, null)) {
            if (cursor != null && cursor.moveToFirst()) {
                int column = cursor.getColumnIndex(OpenableColumns.SIZE);
                if (column >= 0 && !cursor.isNull(column)) return cursor.getLong(column);
            }
        } catch (Exception ignored) {
        }
        return -1;
    }

    @Override
    protected String[] getLibraries() {
        // SDL3 is linked statically into libmain.so; there is no libSDL3.so.
        return new String[]{"main"};
    }

    @Override
    protected String[] getArguments() {
        if (sIso == null) return new String[]{"--help"};
        File files = getFilesDir();
        File elf = new File(files, "SCUS_974.81");
        File memcard = new File(files, "gow2.ps2");
        ArrayList<String> args = new ArrayList<>();
        args.add(elf.getAbsolutePath());
        args.add("--iso-fd");
        args.add(String.valueOf(sIso.getFd()));
        args.add("--iso-size");
        args.add(String.valueOf(sIsoSize));
        args.add("--data-dir");
        args.add(files.getAbsolutePath());
        args.add("--memcard");
        args.add(memcard.getAbsolutePath());
        args.add("--window");
        args.add("--fullscreen");
        if (getSharedPreferences(LauncherActivity.PREFS, MODE_PRIVATE)
                .getBoolean(LauncherActivity.KEY_WIDESCREEN, false)) {
            args.add("--widescreen");
        }
        return args.toArray(new String[0]);
    }
}
