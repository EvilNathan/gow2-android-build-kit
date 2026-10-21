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
import android.widget.TextView;
import android.widget.Toast;

public final class LauncherActivity extends Activity {
    static final String PREFS = "gow2";
    static final String KEY_ISO_URI = "iso_uri";
    static final String KEY_WIDESCREEN = "widescreen";
    static final String KEY_SCALE = "scale";
    static final String KEY_TOUCH_ALPHA = "touch_alpha";
    private static final int REQ_ISO = 1001;

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

        TextView note = new TextView(this);
        note.setText("Changes apply the next time you start the game. Saves are written to the app's memory-card file and survive updates.");
        note.setTextColor(Color.rgb(160, 154, 146));
        note.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11);
        LinearLayout.LayoutParams noteParams = matchWrap();
        noteParams.topMargin = dp(8);
        content.addView(note, noteParams);

        new AlertDialog.Builder(this)
                .setTitle("Settings")
                .setView(content)
                .setPositiveButton("Done", (dialog, which) -> {
                    prefs.edit()
                            .putBoolean(KEY_WIDESCREEN, widescreen.isChecked())
                            .putInt(KEY_SCALE, scales.getCheckedRadioButtonId() == 2 ? 2 : 1)
                            .putFloat(KEY_TOUCH_ALPHA, dim.isChecked() ? 0.55f : 0.86f)
                            .apply();
                    refresh();
                })
                .setNegativeButton("Cancel", null)
                .show();
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
