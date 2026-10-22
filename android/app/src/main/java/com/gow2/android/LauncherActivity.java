package com.gow2.android;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.ContentResolver;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.res.ColorStateList;
import android.graphics.Color;
import android.graphics.LinearGradient;
import android.graphics.Shader;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.RippleDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;
import android.database.Cursor;
import android.text.Spannable;
import android.text.SpannableStringBuilder;
import android.text.style.ForegroundColorSpan;
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
    static final String KEY_SAFE = "safe_mode";
    static final String KEY_FPS30 = "fps_cadence_30";
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
        LogFile.append(this, "[app] launcher opened (log " + LogFile.path(this) + ")");
    }

    private TextView logPathView;

    private View buildUi() {
        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.setBackgroundResource(R.drawable.bg_launcher);
        scroll.setClipToPadding(false);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.HORIZONTAL);
        root.setGravity(Gravity.CENTER_VERTICAL);
        root.setPadding(dp(28), dp(16), dp(28), dp(16));
        scroll.addView(root, new ScrollView.LayoutParams(
                ScrollView.LayoutParams.MATCH_PARENT, ScrollView.LayoutParams.WRAP_CONTENT));

        LinearLayout left = new LinearLayout(this);
        left.setOrientation(LinearLayout.VERTICAL);
        left.setGravity(Gravity.CENTER_VERTICAL);
        root.addView(left, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1.05f));

        View accent = new View(this);
        accent.setBackgroundResource(R.drawable.accent_bar);
        LinearLayout.LayoutParams accentParams = new LinearLayout.LayoutParams(dp(56), dp(3));
        accentParams.bottomMargin = dp(14);
        left.addView(accent, accentParams);

        TextView title = new TextView(this);
        title.setText("GOD OF WAR II");
        title.setTextColor(Color.WHITE);
        title.setTypeface(Typeface.create("serif", Typeface.BOLD));
        title.setTextSize(TypedValue.COMPLEX_UNIT_SP, 38);
        title.setLetterSpacing(0.09f);
        left.addView(title, matchWrap());
        title.post(() -> title.getPaint().setShader(new LinearGradient(
                0, 0, 0, title.getTextSize(),
                0xFFF7E3BC, 0xFFC48C3C, Shader.TileMode.CLAMP)));

        TextView subtitle = new TextView(this);
        subtitle.setText("NATIVE ANDROID PORT  \u00b7  BUILT FROM YOUR OWN DISC");
        subtitle.setTextColor(0xFFC8AE86);
        subtitle.setTextSize(TypedValue.COMPLEX_UNIT_SP, 10);
        subtitle.setLetterSpacing(0.16f);
        LinearLayout.LayoutParams subParams = matchWrap();
        subParams.topMargin = dp(4);
        left.addView(subtitle, subParams);

        TextView hint = new TextView(this);
        hint.setText("Select your legally acquired God of War II (USA) ISO. The disc is read in place - never copied or uploaded.");
        hint.setTextColor(0xFFBBB2A6);
        hint.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        hint.setLineSpacing(dp(2), 1f);
        LinearLayout.LayoutParams hintParams = matchWrap();
        hintParams.topMargin = dp(14);
        left.addView(hint, hintParams);

        status = new TextView(this);
        status.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        LinearLayout.LayoutParams statusParams = matchWrap();
        statusParams.topMargin = dp(16);
        statusParams.bottomMargin = dp(10);
        left.addView(status, statusParams);

        play = makePrimaryButton("PLAY");
        play.setOnClickListener(v -> launchGame());
        left.addView(play, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, dp(60)));

        LinearLayout buttons = new LinearLayout(this);
        buttons.setOrientation(LinearLayout.HORIZONTAL);
        LinearLayout.LayoutParams rowParams = matchWrap();
        rowParams.topMargin = dp(10);
        left.addView(buttons, rowParams);

        choose = makeGhostButton("SELECT ISO");
        choose.setOnClickListener(v -> pickIso());
        buttons.addView(choose, new LinearLayout.LayoutParams(0, dp(50), 1.0f));

        Button settings = makeGhostButton("SETTINGS");
        settings.setOnClickListener(v -> showSettings());
        LinearLayout.LayoutParams settingsParams = new LinearLayout.LayoutParams(0, dp(50), 1.0f);
        settingsParams.leftMargin = dp(10);
        buttons.addView(settings, settingsParams);

        TextView footer = new TextView(this);
        footer.setText("v" + versionName() + "  \u00b7  arm64-v8a  \u00b7  Vulkan via wgpu-native");
        footer.setTextColor(0xFF7E766B);
        footer.setTextSize(TypedValue.COMPLEX_UNIT_SP, 10);
        LinearLayout.LayoutParams footerParams = matchWrap();
        footerParams.topMargin = dp(12);
        left.addView(footer, footerParams);

        LinearLayout right = new LinearLayout(this);
        right.setOrientation(LinearLayout.VERTICAL);
        right.setGravity(Gravity.CENTER_VERTICAL);
        LinearLayout.LayoutParams rightParams = new LinearLayout.LayoutParams(
                0, LinearLayout.LayoutParams.WRAP_CONTENT, 0.95f);
        rightParams.leftMargin = dp(26);
        root.addView(right, rightParams);

        right.addView(makeCard("CONTROLS",
                "Left half of the screen: movement\n"
                        + "Right diamond: face buttons\n"
                        + "START / SELECT strip at the bottom\n"
                        + "Bluetooth / USB gamepad is supported"));

        LinearLayout logCard = makeCard("DIAGNOSTIC LOG", null);
        logPathView = new TextView(this);
        logPathView.setTextColor(0xFFBBB2A6);
        logPathView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 10);
        logPathView.setTypeface(Typeface.MONOSPACE);
        logPathView.setTextIsSelectable(true);
        LinearLayout.LayoutParams pathParams = matchWrap();
        pathParams.topMargin = dp(7);
        logCard.addView(logPathView, pathParams);

        LinearLayout logButtons = new LinearLayout(this);
        logButtons.setOrientation(LinearLayout.HORIZONTAL);
        Button viewLog = makeGhostButton("VIEW LOG");
        viewLog.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        viewLog.setOnClickListener(v -> showLog());
        logButtons.addView(viewLog, new LinearLayout.LayoutParams(0, dp(44), 1f));
        Button copyPath = makeGhostButton("COPY PATH");
        copyPath.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        copyPath.setOnClickListener(v -> {
            ClipboardManager clipboard = (ClipboardManager) getSystemService(CLIPBOARD_SERVICE);
            if (clipboard != null) {
                clipboard.setPrimaryClip(ClipData.newPlainText("GoW2 log", LogFile.path(this)));
                Toast.makeText(this, "Log path copied", Toast.LENGTH_SHORT).show();
            }
        });
        LinearLayout.LayoutParams copyParams = new LinearLayout.LayoutParams(0, dp(44), 1f);
        copyParams.leftMargin = dp(8);
        logButtons.addView(copyPath, copyParams);
        LinearLayout.LayoutParams logButtonParams = matchWrap();
        logButtonParams.topMargin = dp(10);
        logCard.addView(logButtons, logButtonParams);

        if (!LogFile.usingShared(this)) {
            Button allow = makeGhostButton("ALLOW /SDCARD WRITE");
            allow.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
            allow.setOnClickListener(v -> requestSharedStorage());
            LinearLayout.LayoutParams allowParams = new LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.MATCH_PARENT, dp(44));
            allowParams.topMargin = dp(8);
            logCard.addView(allow, allowParams);
        }

        LinearLayout.LayoutParams logCardParams = matchWrap();
        logCardParams.topMargin = dp(12);
        right.addView(logCard, logCardParams);

        return scroll;
    }

    private LinearLayout makeCard(String heading, String body) {
        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setBackgroundResource(R.drawable.card_bg);
        card.setPadding(dp(16), dp(14), dp(16), dp(14));

        TextView headingView = new TextView(this);
        headingView.setText(heading);
        headingView.setTextColor(0xFFE0B36A);
        headingView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11);
        headingView.setLetterSpacing(0.14f);
        card.addView(headingView, matchWrap());

        if (body != null) {
            TextView bodyView = new TextView(this);
            bodyView.setText(body);
            bodyView.setTextColor(0xFFCFC7BC);
            bodyView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11);
            bodyView.setLineSpacing(dp(2), 1f);
            LinearLayout.LayoutParams bodyParams = matchWrap();
            bodyParams.topMargin = dp(7);
            card.addView(bodyView, bodyParams);
        }
        return card;
    }

    private Button makePrimaryButton(String text) {
        Button button = new Button(this);
        button.setText(text);
        button.setTextColor(0xFFF7E3BC);
        button.setTextSize(TypedValue.COMPLEX_UNIT_SP, 20);
        button.setTypeface(Typeface.create("sans-serif-condensed", Typeface.BOLD));
        button.setLetterSpacing(0.18f);
        button.setAllCaps(false);
        GradientDrawable shape = new GradientDrawable(
                GradientDrawable.Orientation.TOP_BOTTOM,
                new int[]{0xFFB8452A, 0xFF741E12});
        shape.setCornerRadius(dp(14));
        shape.setStroke(dp(1), 0xB3E2B86C);
        button.setBackground(new RippleDrawable(ColorStateList.valueOf(0x40FFFFFF), shape, null));
        button.setElevation(dp(6));
        button.setStateListAnimator(null);
        return button;
    }

    private Button makeGhostButton(String text) {
        Button button = new Button(this);
        button.setText(text);
        button.setTextColor(0xFFE8DCC8);
        button.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        button.setTypeface(Typeface.create("sans-serif-condensed", Typeface.BOLD));
        button.setLetterSpacing(0.10f);
        button.setAllCaps(false);
        GradientDrawable shape = new GradientDrawable();
        shape.setColor(0x1FFFFFFF);
        shape.setCornerRadius(dp(12));
        shape.setStroke(dp(1), 0x4DD9A24A);
        button.setBackground(new RippleDrawable(ColorStateList.valueOf(0x33FFFFFF), shape, null));
        button.setStateListAnimator(null);
        return button;
    }

    private void setStatus(String text, boolean ready, boolean warning) {
        SpannableStringBuilder builder = new SpannableStringBuilder("\u25cf  " + text);
        int dot = ready ? 0xFF86D68F : (warning ? 0xFFE0B45A : 0xFFD06A5A);
        builder.setSpan(new ForegroundColorSpan(dot), 0, 1, Spannable.SPAN_EXCLUSIVE_EXCLUSIVE);
        status.setText(builder);
        status.setTextColor(0xFFD6CEC2);
    }

    private void refreshLogPath() {
        if (logPathView == null) return;
        String path = LogFile.path(this);
        boolean shared = LogFile.usingShared(this);
        logPathView.setText((shared ? "" : "Fallback path (grant all-files access for /sdcard/gow2/log.txt):\n")
                + (path == null ? "unavailable" : path));
    }

    private String versionName() {
        try {
            return getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
        } catch (Exception error) {
            return "?";
        }
    }

    private void showLog() {
        String tail = LogFile.tail(this, 48 * 1024);
        if (tail.isEmpty()) tail = "The log is empty. Start the game once, then open this again.";
        TextView text = new TextView(this);
        text.setText(tail);
        text.setTextColor(0xFFD8D2C8);
        text.setTextSize(TypedValue.COMPLEX_UNIT_SP, 10);
        text.setTypeface(Typeface.MONOSPACE);
        text.setTextIsSelectable(true);
        int pad = dp(14);
        text.setPadding(pad, pad, pad, pad);
        ScrollView scroller = new ScrollView(this);
        scroller.addView(text);
        new AlertDialog.Builder(this)
                .setTitle("Log - " + LogFile.path(this))
                .setView(scroller)
                .setPositiveButton("Close", null)
                .show();
    }

    private void requestSharedStorage() {
        if (Build.VERSION.SDK_INT >= 30) {
            try {
                startActivity(new Intent(
                        android.provider.Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                        Uri.parse("package:" + getPackageName())));
                Toast.makeText(this, "Grant \"All files access\", then reopen the app", Toast.LENGTH_LONG).show();
                return;
            } catch (Exception ignored) {
            }
            try {
                startActivity(new Intent(
                        android.provider.Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
                return;
            } catch (Exception ignored) {
            }
        }
        Toast.makeText(this, "Log path: " + LogFile.path(this), Toast.LENGTH_LONG).show();
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
        b.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        b.setAllCaps(false);
        b.setTypeface(Typeface.create("sans-serif-condensed", Typeface.BOLD));
        GradientDrawable shape = new GradientDrawable();
        shape.setColor(color);
        shape.setCornerRadius(dp(12));
        shape.setStroke(dp(1), 0x66D9A24A);
        b.setBackground(new RippleDrawable(ColorStateList.valueOf(0x33FFFFFF), shape, null));
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
        drs.setText("Dynamic resolution (optional)");
        drs.setTextColor(Color.rgb(230, 224, 214));
        drs.setChecked(prefs.getBoolean(KEY_DRS, false));
        LinearLayout.LayoutParams drsParams = matchWrap();
        drsParams.topMargin = dp(8);
        content.addView(drs, drsParams);

        TextView drsHint = new TextView(this);
        drsHint.setText("Off by default: it reconfigures the Android swapchain while the game runs. "
                + "Turn it on if the GPU has headroom problems; PS2 render scale and geometry stay unchanged. "
                + "Works with any Vulkan driver (Adreno, Mali, Xclipse, PowerVR).");
        drsHint.setTextColor(Color.rgb(160, 154, 146));
        drsHint.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11);
        content.addView(drsHint, matchWrap());

        CheckBox fps30 = new CheckBox(this);
        fps30.setText("30 fps cadence (halves the console rate)");
        fps30.setTextColor(Color.rgb(230, 224, 214));
        fps30.setChecked(prefs.getBoolean(KEY_FPS30, false));
        LinearLayout.LayoutParams fpsParams = matchWrap();
        fpsParams.topMargin = dp(8);
        content.addView(fps30, fpsParams);

        TextView fpsHint = new TextView(this);
        fpsHint.setText("For phones that cannot hold 60 fps. Runs the game at a steady 30 Hz instead of "
                + "dropping frames at 60, so motion stays even (the game runs at half speed).");
        fpsHint.setTextColor(Color.rgb(160, 154, 146));
        fpsHint.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11);
        content.addView(fpsHint, matchWrap());

        CheckBox safe = new CheckBox(this);
        safe.setText("Safe mode (disable GPU extras)");
        safe.setTextColor(Color.rgb(230, 224, 214));
        safe.setChecked(prefs.getBoolean(KEY_SAFE, false));
        LinearLayout.LayoutParams safeParams = matchWrap();
        safeParams.topMargin = dp(12);
        content.addView(safe, safeParams);

        TextView safeHint = new TextView(this);
        safeHint.setText("For troubleshooting: ignores the custom driver and turbo clock, forces the "
                + "system driver, 60 Hz and a fixed 1x swapchain. Leave off for normal play.");
        safeHint.setTextColor(Color.rgb(160, 154, 146));
        safeHint.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11);
        content.addView(safeHint, matchWrap());

        final boolean adreno = isAdreno();
        final CheckBox turbo;
        {
            TextView driverLabel = new TextView(this);
            driverLabel.setText("GPU driver (optional, Adreno/Turnip only)");
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
            driverHint.setText(adreno
                    ? "Import an AdrenoTools package (.zip) or a libvulkan*.so, or leave the system driver. "
                      + "A custom driver is never required."
                    : "Not applicable on this GPU: " + Build.MANUFACTURER + " " + Build.MODEL
                      + " uses its system Vulkan driver, which is fully supported. "
                      + "Dynamic resolution, 30 fps cadence and stats work on every vendor.");
            driverHint.setTextColor(Color.rgb(160, 154, 146));
            driverHint.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11);
            LinearLayout.LayoutParams driverHintParams = matchWrap();
            driverHintParams.bottomMargin = dp(6);
            content.addView(driverHint, driverHintParams);

            Button pickDriver = makeButton("SELECT DRIVER (.zip / .so)", Color.rgb(58, 54, 50));
            pickDriver.setOnClickListener(v -> pickDriver());
            pickDriver.setEnabled(adreno);
            pickDriver.setAlpha(adreno ? 1.0f : 0.4f);
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
            turbo.setText("Turbo clock (force maximum GPU clocks, Adreno only)");
            turbo.setTextColor(Color.rgb(230, 224, 214));
            turbo.setChecked(prefs.getBoolean(KEY_TURBO, false));
            turbo.setEnabled(adreno);
            LinearLayout.LayoutParams turboParams = matchWrap();
            turboParams.topMargin = dp(10);
            content.addView(turbo, turboParams);
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
                            .putBoolean(KEY_DRS, drs.isChecked() && !safe.isChecked())
                            .putBoolean(KEY_FPS30, fps30.isChecked() && !safe.isChecked())
                            .putBoolean(KEY_TURBO, turbo != null && turbo.isChecked() && !safe.isChecked())
                            .putBoolean(KEY_SAFE, safe.isChecked())
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
        refreshLogPath();
        Uri uri = savedUri();
        if (uri == null) {
            setStatus("No ISO selected - tap SELECT ISO", false, false);
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
            setStatus("Ready  \u00b7  " + formatSize(size) + "  \u00b7  " + aspect + "  \u00b7  " + scale + "x", true, false);
            play.setEnabled(true);
            play.setAlpha(1.0f);
            choose.setText("CHANGE ISO");
        } else {
            setStatus("The selected file is no longer readable. Select it again.", false, true);
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
        long selectedSize = fileSize(uri);
        LogFile.append(this, "[app] ISO selected: " + displayName(uri) + " (" + selectedSize + " bytes)");
        refresh();
        if (selectedSize <= 0) {
            Toast.makeText(this, "Could not read that file", Toast.LENGTH_LONG).show();
        }
    }

    private void launchGame() {
        if (savedUri() == null) return;
        LogFile.append(this, "[app] Play pressed");
        startActivity(new Intent(this, GameActivity.class));
    }
}
