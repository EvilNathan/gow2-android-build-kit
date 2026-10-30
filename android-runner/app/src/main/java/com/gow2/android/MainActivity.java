package com.gow2.android;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.Spinner;
import android.widget.ArrayAdapter;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * Launcher screen: imports the disc image, extracts the boot ELF and starts {@link GameActivity}.
 *
 * <p>It never loads the native engine itself; only {@link NativeBridge#nativeExtractDisc} needs the
 * library, so settings and the ISO import keep working even when the .so failed to load.
 */
public final class MainActivity extends Activity {

    private static final String TAG = "PS2X";
    private static final int REQUEST_ISO = 1001;

    private final Handler handler = new Handler(Looper.getMainLooper());
    private final ExecutorService worker = Executors.newSingleThreadExecutor();

    private SettingsStore store;
    private Settings settings;

    private TextView status;
    private Button startButton;
    private boolean busy;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        store = new SettingsStore(this);
        settings = store.get();
        buildUi();
        refreshStatus();
    }

    @Override
    protected void onResume() {
        super.onResume();
        refreshStatus();
    }

    @Override
    protected void onDestroy() {
        worker.shutdownNow();
        super.onDestroy();
    }

    // ---------------------------------------------------------------------
    // UI
    // ---------------------------------------------------------------------
    private void buildUi() {
        final LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(Color.rgb(18, 18, 22));
        root.setPadding(dp(24), dp(24), dp(24), dp(24));

        final TextView title = new TextView(this);
        title.setText("GOD OF WAR II");
        title.setTextColor(Color.WHITE);
        title.setTextSize(22.0f);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        root.addView(title);

        final TextView subtitle = new TextView(this);
        subtitle.setText("Android arm64 build kit \u2014 impor ISO milikmu sendiri");
        subtitle.setTextColor(Color.rgb(170, 170, 180));
        subtitle.setTextSize(12.0f);
        root.addView(subtitle);

        status = new TextView(this);
        status.setTextColor(Color.rgb(210, 210, 220));
        status.setTextSize(13.0f);
        status.setTypeface(Typeface.MONOSPACE);
        status.setPadding(0, dp(20), 0, dp(20));
        root.addView(status);

        startButton = addButton(root, "Mulai", view -> startGame());
        addButton(root, "Impor ISO", view -> pickIso());
        addButton(root, "Pengaturan", view -> showSettings());

        if (!NativeBridge.isLoaded()) {
            final TextView warning = new TextView(this);
            warning.setText("Library native tidak termuat:\n" + NativeBridge.loadError());
            warning.setTextColor(Color.rgb(255, 150, 120));
            warning.setTextSize(12.0f);
            warning.setPadding(0, dp(16), 0, 0);
            root.addView(warning);
        }

        final ScrollView scroll = new ScrollView(this);
        scroll.addView(root);
        setContentView(scroll);
    }

    private Button addButton(LinearLayout parent, String label, View.OnClickListener listener) {
        final Button button = new Button(this);
        button.setText(label);
        button.setAllCaps(false);
        button.setOnClickListener(listener);
        final LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        params.topMargin = dp(8);
        parent.addView(button, params);
        return button;
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    private void refreshStatus() {
        final File iso = SettingsStore.isoFile(this);
        final File elf = SettingsStore.bootElfFile(this);
        final boolean isoReady = DiscInstaller.isReadable(iso);
        final boolean elfReady = DiscInstaller.isReadable(elf);

        final StringBuilder text = new StringBuilder();
        text.append("ISO   : ").append(isoReady ? DiscInstaller.formatBytes(iso.length()) : "belum ada")
                .append('\n');
        text.append("Boot  : ").append(elfReady ? elf.getName() : "belum diekstrak").append('\n');
        text.append("Folder: ").append(SettingsStore.filesPath(this)).append('\n');
        text.append("Native: ").append(NativeBridge.isLoaded() ? "siap" : "gagal");
        status.setText(text.toString());

        startButton.setEnabled(isoReady && !busy);
    }

    // ---------------------------------------------------------------------
    // Start
    // ---------------------------------------------------------------------
    private void startGame() {
        final File iso = SettingsStore.isoFile(this);
        final File elf = SettingsStore.bootElfFile(this);
        if (!DiscInstaller.isReadable(iso)) {
            Toast.makeText(this, "Impor ISO God of War II dulu", Toast.LENGTH_SHORT).show();
            return;
        }
        if (DiscInstaller.isReadable(elf)) {
            launchGame();
            return;
        }
        extractBootData(iso);
    }

    private void launchGame() {
        settings = store.get();
        startActivity(GameActivity.createIntent(this, settings));
    }

    /** Extracts the boot ELF from the imported ISO through the native reader. */
    private void extractBootData(File iso) {
        if (!NativeBridge.isLoaded()) {
            Toast.makeText(this, "Library native tidak termuat", Toast.LENGTH_LONG).show();
            return;
        }
        setBusy(true, "Mengekstrak boot ELF...");
        final String isoPath = iso.getAbsolutePath();
        final String filesDir = SettingsStore.filesPath(this);
        worker.execute(() -> {
            String message = "";
            boolean ok = false;
            try {
                final String json = NativeBridge.nativeExtractDisc(isoPath, filesDir);
                final org.json.JSONObject object = new org.json.JSONObject(json);
                ok = object.optBoolean("ok", false);
                message = ok
                        ? "Boot ELF " + object.optString("elf", "") + " (SHA1 cocok: " + object.optBoolean("match", false) + ")"
                        : object.optString("error", "gagal mengekstrak ELF");
            } catch (Exception error) {
                Log.e(TAG, "nativeExtractDisc failed", error);
                message = String.valueOf(error.getMessage());
            }
            final boolean extracted = ok;
            final String result = message;
            handler.post(() -> {
                setBusy(false, null);
                refreshStatus();
                if (extracted) {
                    Toast.makeText(this, result, Toast.LENGTH_SHORT).show();
                    launchGame();
                } else {
                    new AlertDialog.Builder(this)
                            .setTitle("Ekstraksi gagal")
                            .setMessage(result)
                            .setPositiveButton("OK", null)
                            .show();
                }
            });
        });
    }

    private void setBusy(boolean value, String message) {
        busy = value;
        if (message != null) {
            status.setText(message);
        }
        startButton.setEnabled(!value && DiscInstaller.isReadable(SettingsStore.isoFile(this)));
    }

    // ---------------------------------------------------------------------
    // ISO import
    // ---------------------------------------------------------------------
    private void pickIso() {
        final Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        intent.putExtra(Intent.EXTRA_MIME_TYPES, new String[]{"application/x-iso9660-image", "application/octet-stream"});
        try {
            startActivityForResult(intent, REQUEST_ISO);
        } catch (RuntimeException error) {
            Log.e(TAG, "no document picker", error);
            Toast.makeText(this, "Tidak ada aplikasi pemilih berkas", Toast.LENGTH_LONG).show();
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != REQUEST_ISO || resultCode != RESULT_OK || data == null || data.getData() == null) {
            return;
        }
        final Uri uri = data.getData();
        try {
            getContentResolver().takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (SecurityException error) {
            Log.w(TAG, "cannot persist the ISO permission", error);
        }
        importIso(uri);
    }

    private void importIso(Uri uri) {
        final File destination = SettingsStore.isoFile(this);
        setBusy(true, "Menyalin ISO...");

        final AlertDialog progress = new AlertDialog.Builder(this)
                .setTitle("Mengimpor ISO")
                .setMessage("Menyiapkan...")
                .setCancelable(false)
                .create();
        progress.show();

        worker.execute(() -> {
            final DiscInstaller.Result result = DiscInstaller.install(this, uri, destination, (copied, total) -> {
                final String percent = total > 0
                        ? String.format(java.util.Locale.US, "%d%%", Math.min(100, (copied * 100) / total))
                        : DiscInstaller.formatBytes(copied);
                final String text = "Menyalin " + DiscInstaller.formatBytes(copied)
                        + (total > 0 ? " / " + DiscInstaller.formatBytes(total) : "") + "  " + percent;
                handler.post(() -> progress.setMessage(text));
            });

            handler.post(() -> {
                progress.dismiss();
                setBusy(false, null);
                if (result.ok) {
                    settings.isoUri = uri.toString();
                    store.save(settings);
                    refreshStatus();
                    // Always re-extract: the imported image may be a different revision.
                    extractBootData(destination);
                } else {
                    new AlertDialog.Builder(this)
                            .setTitle("Impor gagal")
                            .setMessage(result.error)
                            .setPositiveButton("OK", null)
                            .show();
                }
            });
        });
    }

    // ---------------------------------------------------------------------
    // Settings
    // ---------------------------------------------------------------------
    private void showSettings() {
        final Settings draft = store.get();
        final LinearLayout content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setPadding(dp(20), dp(12), dp(20), dp(12));

        final RadioGroup presetGroup = new RadioGroup(this);
        presetGroup.setOrientation(RadioGroup.HORIZONTAL);
        addRadio(presetGroup, "Performa", Settings.PRESET_PERFORMANCE);
        addRadio(presetGroup, "Seimbang", Settings.PRESET_BALANCED);
        addRadio(presetGroup, "Kualitas", Settings.PRESET_QUALITY);
        checkRadio(presetGroup, draft.preset);
        content.addView(label("Grafik"));
        content.addView(presetGroup);

        final RadioGroup aspectGroup = new RadioGroup(this);
        aspectGroup.setOrientation(RadioGroup.HORIZONTAL);
        addRadio(aspectGroup, "Penuh", Settings.ASPECT_FILL);
        addRadio(aspectGroup, "4:3", Settings.ASPECT_4X3);
        addRadio(aspectGroup, "Fit", Settings.ASPECT_FIT);
        checkRadio(aspectGroup, draft.aspect);
        content.addView(label("Aspek layar"));
        content.addView(aspectGroup);

        final CheckBox vsync = new CheckBox(this);
        vsync.setText("VSync");
        vsync.setChecked(draft.vsync);
        content.addView(vsync);

        final SeekBar volume = addSlider(content, "Volume", 0, 100, Math.round(draft.volume * 100.0f));
        final SeekBar touchScale = addSlider(content, "Ukuran tombol", 70, 160, Math.round(draft.touchScale * 100.0f));
        final SeekBar touchOpacity = addSlider(content, "Opasitas tombol", 15, 100, Math.round(draft.touchOpacity * 100.0f));
        final SeekBar deadzone = addSlider(content, "Deadzone", 0, 45, Math.round(draft.deadzone * 100.0f));

        final CheckBox leftHanded = new CheckBox(this);
        leftHanded.setText("Kontrol kidal");
        leftHanded.setChecked(draft.leftHanded);
        content.addView(leftHanded);

        final Spinner logLevel = new Spinner(this);
        final String[] levels = {"info", "debug", "warn", "error"};
        logLevel.setAdapter(new ArrayAdapter<>(this, android.R.layout.simple_spinner_dropdown_item, levels));
        for (int i = 0; i < levels.length; i++) {
            if (levels[i].equals(draft.logLevel)) {
                logLevel.setSelection(i);
                break;
            }
        }
        content.addView(label("Level log"));
        content.addView(logLevel);

        final ScrollView scroll = new ScrollView(this);
        scroll.addView(content);

        new AlertDialog.Builder(this)
                .setTitle("Pengaturan")
                .setView(scroll)
                .setNegativeButton("Batal", null)
                .setPositiveButton("Simpan", (dialog, which) -> {
                    draft.preset = (String) presetGroup.getTag();
                    draft.applyPreset(draft.preset);
                    draft.aspect = (String) aspectGroup.getTag();
                    draft.vsync = vsync.isChecked();
                    draft.volume = volume.getProgress() / 100.0f;
                    draft.touchScale = touchScale.getProgress() / 100.0f;
                    draft.touchOpacity = touchOpacity.getProgress() / 100.0f;
                    draft.deadzone = deadzone.getProgress() / 100.0f;
                    draft.leftHanded = leftHanded.isChecked();
                    draft.logLevel = (String) logLevel.getSelectedItem();
                    store.save(draft);
                    settings = store.get();
                })
                .show();
    }

    private TextView label(String text) {
        final TextView view = new TextView(this);
        view.setText(text);
        view.setTextColor(Color.rgb(190, 190, 200));
        view.setTextSize(12.0f);
        view.setPadding(0, dp(12), 0, dp(2));
        return view;
    }

    private void addRadio(RadioGroup group, String text, String value) {
        final RadioButton button = new RadioButton(this);
        button.setText(text);
        button.setTag(value);
        group.addView(button);
    }

    private void checkRadio(RadioGroup group, String value) {
        for (int i = 0; i < group.getChildCount(); i++) {
            final RadioButton button = (RadioButton) group.getChildAt(i);
            if (value.equals(button.getTag())) {
                button.setChecked(true);
                group.setTag(value);
                break;
            }
        }
        group.setOnCheckedChangeListener((radio, checkedId) -> {
            final RadioButton button = radio.findViewById(checkedId);
            if (button != null) {
                radio.setTag(button.getTag());
            }
        });
    }

    private SeekBar addSlider(LinearLayout parent, String text, int min, int max, int value) {
        final TextView title = label(text + "  " + value + "%");
        parent.addView(title);
        final SeekBar bar = new SeekBar(this);
        bar.setMax(max - min);
        bar.setProgress(Math.max(0, Math.min(max - min, value - min)));
        bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                title.setText(text + "  " + (progress + min) + "%");
            }

            @Override
            public void onStartTrackingTouch(SeekBar seekBar) {
            }

            @Override
            public void onStopTrackingTouch(SeekBar seekBar) {
            }
        });
        parent.addView(bar);
        return bar;
    }
}
