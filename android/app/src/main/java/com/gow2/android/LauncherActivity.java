package com.gow2.android;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ContentResolver;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;
import android.database.Cursor;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

public final class LauncherActivity extends Activity {
    static final String PREFS = "gow2";
    static final String KEY_ISO_URI = "iso_uri";
    static final String KEY_WIDESCREEN = "widescreen";
    static final String KEY_SCALE = "scale";
    static final String KEY_TOUCH_ALPHA = "touch_alpha";
    static final String KEY_DRIVER_PATH = "driver_path";
    static final String KEY_TURBO = "turbo_clock";
    static final String KEY_DRS = "dynamic_resolution";
    private static final int REQ_ISO = 1001;
    private static final int REQ_DRIVER = 1002;

    private TextView status;
    private Button play;
    private Button choose;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(buildUi());
        refresh();
    }

    private View buildUi() {
        final int pad = dp(28);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setGravity(Gravity.CENTER_VERTICAL);
        root.setPadding(pad, dp(12), pad, dp(12));
        GradientDrawable background = new GradientDrawable(
                GradientDrawable.Orientation.TL_BR,
                new int[]{Color.rgb(24, 10, 8), Color.rgb(72, 18, 12), Color.rgb(10, 8, 8)});
        root.setBackground(background);

        TextView title = new TextView(this);
        title.setText("GOD OF WAR II");
        title.setTextColor(Color.rgb(231, 201, 151));
        title.setTypeface(Typeface.create("serif", Typeface.BOLD));
        title.setTextSize(TypedValue.COMPLEX_UNIT_SP, 38);
        title.setLetterSpacing(0.08f);
        title.setGravity(Gravity.CENTER_HORIZONTAL);
        root.addView(title, matchWrap());

        TextView subtitle = new TextView(this);
        subtitle.setText("Native Android port");
        subtitle.setTextColor(Color.rgb(188, 164, 130));
        subtitle.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        subtitle.setGravity(Gravity.CENTER_HORIZONTAL);
        LinearLayout.LayoutParams subParams = matchWrap();
        subParams.topMargin = dp(2);
        root.addView(subtitle, subParams);

        TextView hint = new TextView(this);
        hint.setText("Select your own legally acquired God of War II (USA) ISO. The game is read directly from the file; it is never copied or uploaded.");
        hint.setTextColor(Color.rgb(190, 184, 176));
        hint.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        hint.setGravity(Gravity.CENTER_HORIZONTAL);
        LinearLayout.LayoutParams hintParams = matchWrap();
        hintParams.topMargin = dp(18);
        hintParams.bottomMargin = dp(14);
        root.addView(hint, hintParams);

        status = new TextView(this);
        status.setTextColor(Color.rgb(210, 205, 196));
        status.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        status.setGravity(Gravity.CENTER_HORIZONTAL);
        LinearLayout.LayoutParams statusParams = matchWrap();
        statusParams.bottomMargin = dp(14);
        root.addView(status, statusParams);

        play = makeButton("PLAY", Color.rgb(122, 34, 24));
        play.setOnClickListener(v -> launchGame());
        root.addView(play, buttonParams());

        LinearLayout buttons = new LinearLayout(this);
        buttons.setOrientation(LinearLayout.HORIZONTAL);
        LinearLayout.LayoutParams rowParams = matchWrap();
        rowParams.topMargin = dp(10);
        root.addView(buttons, rowParams);

        choose = makeButton("SELECT ISO", Color.rgb(58, 54, 50));
        choose.setOnClickListener(v -> pickIso());
        LinearLayout.LayoutParams chooseParams = new LinearLayout.LayoutParams(
                0, dp(54), 1.0f);
        buttons.addView(choose, chooseParams);

        Button settings = makeButton("SETTINGS", Color.rgb(42, 40, 38));
        settings.setOnClickListener(v -> showSettings());
        LinearLayout.LayoutParams settingsParams = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT, dp(54));
        settingsParams.leftMargin = dp(10);
        buttons.addView(settings, settingsParams);

        TextView controls = new TextView(this);
        controls.setText("Touch: left side = movement, right diamond = face buttons, START/SELECT at the bottom. A Bluetooth/USB gamepad is supported.");
        controls.setTextColor(Color.rgb(150, 145, 138));
        controls.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11);
        controls.setGravity(Gravity.CENTER_HORIZONTAL);
        LinearLayout.LayoutParams controlsParams = matchWrap();
        controlsParams.topMargin = dp(18);
        root.addView(controls, controlsParams);

        return root;
    }

    private LinearLayout.LayoutParams matchWrap() {
        return new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
    }

    private LinearLayout.LayoutParams buttonParams() {
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, dp(54));
        return p;
    }

    private Button makeButton(String text, int color) {
        Button b = new Button(this);
        b.setText(text);
        b.setTextColor(Color.WHITE);
        b.setTextSize(TypedValue.COMPLEX_UNIT_SP, 17);
        b.setAllCaps(false);
        b.setTypeface(Typeface.DEFAULT_BOLD);
        GradientDrawable shape = new GradientDrawable();
        shape.setColor(color);
        shape.setCornerRadius(dp(8));
        shape.setStroke(dp(1), Color.rgb(205, 160, 92));
        b.setBackground(shape);
        b.setStateListAnimator(null);
        return b;
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    private Uri savedUri() {
        SharedPreferences prefs = getSharedPreferences(PREFS, MODE_PRIVATE);
        String value = prefs.getString(KEY_ISO_URI, null);
        return value == null ? null : Uri.parse(value);
    }

    private void showSettings() {
        SharedPreferences prefs = getSharedPreferences(PREFS, MODE_PRIVATE);
        final int pad = dp(20);

        LinearLayout content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setPadding(pad, dp(8), pad, 0);

        CheckBox widescreen = new CheckBox(this);
        widescreen.setText("Widescreen (16:9)");
        widescreen.setTextColor(Color.rgb(230, 224, 214));
        widescreen.setChecked(prefs.getBoolean(KEY_WIDESCREEN, false));
        content.addView(widescreen, matchWrap());

        TextView scaleLabel = new TextView(this);
        scaleLabel.setText("Render scale");
        scaleLabel.setTextColor(Color.rgb(205, 170, 120));
        scaleLabel.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
        LinearLayout.LayoutParams labelParams = matchWrap();
        labelParams.topMargin = dp(12);
        content.addView(scaleLabel, labelParams);

        RadioGroup scales = new RadioGroup(this);
        scales.setOrientation(RadioGroup.VERTICAL);
        RadioButton one = new RadioButton(this);
        one.setText("1x (recommended)");
        one.setId(1);
        one.setTextColor(Color.rgb(230, 224, 214));
        RadioButton two = new RadioButton(this);
        two.setText("2x (sharper, slower)");
        two.setId(2);
        two.setTextColor(Color.rgb(230, 224, 214));
        scales.addView(one);
        scales.addView(two);
        scales.check(prefs.getInt(KEY_SCALE, 1) == 2 ? 2 : 1);
        content.addView(scales, matchWrap());

        CheckBox dim = new CheckBox(this);
        dim.setText("Dim touch controls");
        dim.setTextColor(Color.rgb(230, 224, 214));
        dim.setChecked(prefs.getFloat(KEY_TOUCH_ALPHA, 0.86f) < 0.7f);
        LinearLayout.LayoutParams dimParams = matchWrap();
        dimParams.topMargin = dp(8);
        content.addView(dim, dimParams);

        CheckBox drs = new CheckBox(this);
        drs.setText("Dynamic resolution (recommended)");
        drs.setTextColor(Color.rgb(230, 224, 214));
        drs.setChecked(prefs.getBoolean(KEY_DRS, true));
        LinearLayout.LayoutParams drsParams = matchWrap();
        drsParams.topMargin = dp(8);
        content.addView(drs, drsParams);

        TextView drsHint = new TextView(this);
        drsHint.setText("Lowers only the Android swapchain scale when the GPU misses frame time. "
                + "PS2 render scale and image geometry stay unchanged; Mali and Adreno both use it.");
        drsHint.setTextColor(Color.rgb(160, 154, 146));
        drsHint.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11);
        content.addView(drsHint, matchWrap());

        final CheckBox turbo;
        if (isAdreno()) {
            TextView driverLabel = new TextView(this);
            driverLabel.setText("GPU driver (Adreno)");
            driverLabel.setTextColor(Color.rgb(205, 170, 120));
            driverLabel.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
            LinearLayout.LayoutParams driverLabelParams = matchWrap();
            driverLabelParams.topMargin = dp(16);
            content.addView(driverLabel, driverLabelParams);

            String saved = prefs.getString(KEY_DRIVER_PATH, null);
            TextView driverStatus = new TextView(this);
            driverStatus.setText(saved == null ? "Active: system Vulkan driver"
                    : "Active: " + new File(saved).getName());
            driverStatus.setTextColor(Color.rgb(230, 224, 214));
            driverStatus.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
            content.addView(driverStatus, matchWrap());

            TextView driverHint = new TextView(this);
            driverHint.setText("Optional. Import an AdrenoTools driver package (.zip) or a libvulkan*.so file. "
                    + "Leave this on the system driver if the custom one is slower or unstable.");
            driverHint.setTextColor(Color.rgb(160, 154, 146));
            driverHint.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11);
            LinearLayout.LayoutParams driverHintParams = matchWrap();
            driverHintParams.bottomMargin = dp(6);
            content.addView(driverHint, driverHintParams);

            Button pickDriver = makeButton("SELECT DRIVER (.zip / .so)", Color.rgb(58, 54, 50));
            pickDriver.setOnClickListener(v -> pickDriver());
            LinearLayout.LayoutParams pickParams = new LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.MATCH_PARENT, dp(48));
            content.addView(pickDriver, pickParams);

            Button systemDriver = makeButton("USE SYSTEM DRIVER", Color.rgb(42, 40, 38));
            systemDriver.setOnClickListener(v -> {
                prefs.edit().remove(KEY_DRIVER_PATH).apply();
                driverStatus.setText("Active: system Vulkan driver");
                Toast.makeText(this, "The system Vulkan driver will be used next start", Toast.LENGTH_LONG).show();
            });
            LinearLayout.LayoutParams systemParams = new LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.MATCH_PARENT, dp(48));
            systemParams.topMargin = dp(6);
            content.addView(systemDriver, systemParams);

            turbo = new CheckBox(this);
            turbo.setText("Turbo clock (force maximum GPU clocks)");
            turbo.setTextColor(Color.rgb(230, 224, 214));
            turbo.setChecked(prefs.getBoolean(KEY_TURBO, false));
            LinearLayout.LayoutParams turboParams = matchWrap();
            turboParams.topMargin = dp(10);
            content.addView(turbo, turboParams);
        } else {
            turbo = null;
        }

        TextView note = new TextView(this);
        note.setText("Changes apply the next time you start the game. Saves are written to the app's memory-card file and survive updates.");
        note.setTextColor(Color.rgb(160, 154, 146));
        note.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11);
        LinearLayout.LayoutParams noteParams = matchWrap();
        noteParams.topMargin = dp(8);
        content.addView(note, noteParams);

        ScrollView scroll = new ScrollView(this);
        scroll.addView(content);

        new AlertDialog.Builder(this)
                .setTitle("Settings")
                .setView(scroll)
                .setPositiveButton("Done", (dialog, which) -> {
                    prefs.edit()
                            .putBoolean(KEY_WIDESCREEN, widescreen.isChecked())
                            .putInt(KEY_SCALE, scales.getCheckedRadioButtonId() == 2 ? 2 : 1)
                            .putFloat(KEY_TOUCH_ALPHA, dim.isChecked() ? 0.55f : 0.86f)
                            .putBoolean(KEY_DRS, drs.isChecked())
                            .putBoolean(KEY_TURBO, turbo != null && turbo.isChecked())
                            .apply();
                    refresh();
                })
                .setNegativeButton("Cancel", null)
                .show();
    }

    static boolean isAdreno() {
        if (new File("/dev/kgsl-3d0").exists()) return true;
        String hardware = (Build.HARDWARE + " " + Build.BOARD + " " + Build.DEVICE).toLowerCase(Locale.US);
        return hardware.contains("qcom") || hardware.contains("msm") || hardware.contains("adreno");
    }

    private void pickDriver() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        startActivityForResult(intent, REQ_DRIVER);
    }

    private void importDriver(Uri uri) {
        String display = displayName(uri);
        if (display == null) display = "driver";
        String safe = display.replaceAll("[^A-Za-z0-9._-]", "_");
        // Keep the folder inside files/gpu-drivers even for names like "..".
        safe = safe.replaceAll("^\\.+", "");
        if (safe.isEmpty()) safe = "driver";
        File dir = new File(getFilesDir(), "gpu-drivers/" + safe);
        deleteTree(dir);
        if (!dir.mkdirs() && !dir.isDirectory()) {
            Toast.makeText(this, "Could not create the driver folder", Toast.LENGTH_LONG).show();
            return;
        }
        File chosen = null;
        try {
            String lower = display.toLowerCase(Locale.US);
            if (lower.endsWith(".zip")) {
                try (InputStream raw = getContentResolver().openInputStream(uri);
                     ZipInputStream zip = new ZipInputStream(raw)) {
                    ZipEntry entry;
                    while ((entry = zip.getNextEntry()) != null) {
                        String entryName = entry.getName();
                        String entryLower = entryName.toLowerCase(Locale.US);
                        if (!entryLower.endsWith(".so")) continue;
                        File out = new File(dir, new File(entryName).getName());
                        try (OutputStream stream = new FileOutputStream(out)) {
                            copyStream(zip, stream);
                        }
                        if (chosen == null || out.getName().toLowerCase(Locale.US).contains("vulkan")) chosen = out;
                    }
                }
            } else if (lower.endsWith(".so")) {
                File out = new File(dir, safe.endsWith(".so") ? safe : safe + ".so");
                try (InputStream in = getContentResolver().openInputStream(uri);
                     OutputStream stream = new FileOutputStream(out)) {
                    copyStream(in, stream);
                }
                chosen = out;
            } else {
                Toast.makeText(this, "Select a .zip driver package or a .so library", Toast.LENGTH_LONG).show();
                return;
            }
        } catch (Exception e) {
            Toast.makeText(this, "Driver import failed: " + e.getMessage(), Toast.LENGTH_LONG).show();
            return;
        }
        if (chosen == null) {
            Toast.makeText(this, "No driver library found in that package", Toast.LENGTH_LONG).show();
            return;
        }
        getSharedPreferences(PREFS, MODE_PRIVATE).edit().putString(KEY_DRIVER_PATH, chosen.getAbsolutePath()).apply();
        Toast.makeText(this, "Driver imported: " + chosen.getName() + ". It is used the next time you start the game.", Toast.LENGTH_LONG).show();
    }

    private static void copyStream(InputStream in, OutputStream out) throws Exception {
        byte[] buffer = new byte[1 << 16];
        int read;
        while ((read = in.read(buffer)) > 0) out.write(buffer, 0, read);
    }

    private static void deleteTree(File file) {
        if (file == null || !file.exists()) return;
        File[] children = file.listFiles();
        if (children != null) for (File child : children) deleteTree(child);
        file.delete();
    }

    private String displayName(Uri uri) {
        try (Cursor cursor = getContentResolver().query(uri, new String[]{OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (cursor != null && cursor.moveToFirst()) {
                int column = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                if (column >= 0 && !cursor.isNull(column)) return cursor.getString(column);
            }
        } catch (Exception ignored) {
        }
        String path = uri.getLastPathSegment();
        return path == null ? null : new File(path).getName();
    }

    private void refresh() {
        Uri uri = savedUri();
        if (uri == null) {
            status.setText("No ISO selected");
            play.setEnabled(false);
            play.setAlpha(0.45f);
            choose.setText("SELECT ISO");
            return;
        }
        long size = fileSize(uri);
        if (size > 0) {
            SharedPreferences prefs = getSharedPreferences(PREFS, MODE_PRIVATE);
            String aspect = prefs.getBoolean(KEY_WIDESCREEN, false) ? "16:9" : "4:3";
            int scale = prefs.getInt(KEY_SCALE, 1);
            status.setText("Ready: " + formatSize(size) + "  •  " + aspect + "  •  " + scale + "x");
            play.setEnabled(true);
            play.setAlpha(1.0f);
            choose.setText("CHANGE ISO");
        } else {
            status.setText("The selected file is no longer readable. Select it again.");
            play.setEnabled(false);
            play.setAlpha(0.45f);
            choose.setText("SELECT ISO");
        }
    }

    private long fileSize(Uri uri) {
        try (ParcelFileDescriptor pfd = getContentResolver().openFileDescriptor(uri, "r")) {
            if (pfd == null) return -1;
            long size = pfd.getStatSize();
            if (size > 0) return size;
        } catch (Exception ignored) {
        }
        try (Cursor cursor = getContentResolver().query(uri, new String[]{OpenableColumns.SIZE}, null, null, null)) {
            if (cursor != null && cursor.moveToFirst()) {
                int column = cursor.getColumnIndex(OpenableColumns.SIZE);
                if (column >= 0 && !cursor.isNull(column)) return cursor.getLong(column);
            }
        } catch (Exception ignored) {
        }
        return -1;
    }

    private static String formatSize(long bytes) {
        if (bytes >= 1024L * 1024L * 1024L) return String.format(java.util.Locale.US, "%.1f GB", bytes / (1024.0 * 1024.0 * 1024.0));
        return String.format(java.util.Locale.US, "%.0f MB", bytes / (1024.0 * 1024.0));
    }

    private void pickIso() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        startActivityForResult(intent, REQ_ISO);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQ_DRIVER) {
            if (resultCode == RESULT_OK && data != null && data.getData() != null) importDriver(data.getData());
            return;
        }
        if (requestCode != REQ_ISO || resultCode != RESULT_OK || data == null || data.getData() == null) return;
        Uri uri = data.getData();
        try {
            getContentResolver().takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (Exception ignored) {
        }
        getSharedPreferences(PREFS, MODE_PRIVATE).edit().putString(KEY_ISO_URI, uri.toString()).apply();
        refresh();
        if (fileSize(uri) <= 0) {
            Toast.makeText(this, "Could not read that file", Toast.LENGTH_LONG).show();
        }
    }

    private void launchGame() {
        if (savedUri() == null) return;
        startActivity(new Intent(this, GameActivity.class));
    }
}
