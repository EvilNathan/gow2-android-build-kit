package com.gow2.android;

import android.Manifest;
import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.util.Log;
import android.view.Gravity;
import android.view.View;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.Spinner;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * Launcher screen: Spartan aesthetic UI with flexible ISO selection (direct file pick,
 * auto-scan /sdcard/gow2, or copy import), single-file session logging, and boot ELF extraction.
 */
public final class MainActivity extends Activity {

    private static final String TAG = "PS2X";
    private static final int REQUEST_ISO_DIRECT = 1001;
    private static final int REQUEST_ISO_IMPORT = 1002;
    private static final int REQUEST_STORAGE_PERMS = 1003;

    private final Handler handler = new Handler(Looper.getMainLooper());
    private final ExecutorService worker = Executors.newSingleThreadExecutor();

    private SettingsStore store;
    private com.gow2.android.Settings settings;

    private TextView statusBadge;
    private TextView isoDetailText;
    private TextView elfDetailText;
    private TextView logDetailText;
    private Button startButton;
    private boolean busy;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        store = new SettingsStore(this);
        settings = store.get();
        checkAndRequestStoragePermissions();
        buildUi();
        autoDetectIsoIfNeeded();
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
    // Permissions
    // ---------------------------------------------------------------------
    private void checkAndRequestStoragePermissions() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            if (!Environment.isExternalStorageManager()) {
                Log.i(TAG, "Requesting All Files Access for /sdcard/gow2");
                // Informational prompt so user knows why permission is needed
                try {
                    Intent intent = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION);
                    intent.setData(Uri.parse("package:" + getPackageName()));
                    startActivity(intent);
                } catch (Exception error) {
                    try {
                        Intent intent = new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION);
                        startActivity(intent);
                    } catch (Exception ignored) {
                    }
                }
            }
        } else if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            if (checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE) != PackageManager.PERMISSION_GRANTED) {
                requestPermissions(new String[]{
                        Manifest.permission.READ_EXTERNAL_STORAGE,
                        Manifest.permission.WRITE_EXTERNAL_STORAGE
                }, REQUEST_STORAGE_PERMS);
            }
        }
    }

    // ---------------------------------------------------------------------
    // UI Construction (Spartan / God of War Theme)
    // ---------------------------------------------------------------------
    private void buildUi() {
        final ScrollView scroll = new ScrollView(this);
        scroll.setBackgroundColor(Color.rgb(14, 14, 18)); // Obsidian slate

        final LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(dp(28), dp(24), dp(28), dp(28));

        // 1. Header Banner
        final LinearLayout header = new LinearLayout(this);
        header.setOrientation(LinearLayout.VERTICAL);
        header.setPadding(0, 0, 0, dp(16));

        final TextView title = new TextView(this);
        title.setText("GOD OF WAR II");
        title.setTextColor(Color.rgb(235, 175, 60)); // Spartan Gold
        title.setTextSize(26.0f);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        title.setLetterSpacing(0.08f);
        header.addView(title);

        final TextView subtitle = new TextView(this);
        subtitle.setText("AOT Native Recompiler  \u2022  Vulkan 1.1 GS  \u2022  arm64-v8a");
        subtitle.setTextColor(Color.rgb(190, 155, 100)); // Bronze
        subtitle.setTextSize(12.0f);
        subtitle.setPadding(0, dp(4), 0, 0);
        header.addView(subtitle);

        root.addView(header);

        // 2. Status & Details Card
        final LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setPadding(dp(20), dp(18), dp(20), dp(18));

        final GradientDrawable cardBg = new GradientDrawable();
        cardBg.setColor(Color.rgb(22, 22, 30));
        cardBg.setCornerRadius(dp(12));
        cardBg.setStroke(dp(1), Color.rgb(85, 30, 40)); // Crimson dark border
        card.setBackground(cardBg);

        // Status pill row
        final LinearLayout statusRow = new LinearLayout(this);
        statusRow.setOrientation(LinearLayout.HORIZONTAL);
        statusRow.setGravity(Gravity.CENTER_VERTICAL);

        final TextView statusTitle = new TextView(this);
        statusTitle.setText("STATUS SISTEM:");
        statusTitle.setTextColor(Color.rgb(160, 160, 175));
        statusTitle.setTextSize(11.0f);
        statusTitle.setTypeface(Typeface.DEFAULT_BOLD);
        statusRow.addView(statusTitle);

        statusBadge = new TextView(this);
        statusBadge.setText("MEMERIKSA...");
        statusBadge.setTextSize(11.0f);
        statusBadge.setTypeface(Typeface.DEFAULT_BOLD);
        statusBadge.setPadding(dp(10), dp(3), dp(10), dp(3));
        final LinearLayout.LayoutParams badgeParams = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        badgeParams.leftMargin = dp(12);
        statusRow.addView(statusBadge, badgeParams);

        card.addView(statusRow);

        // Divider
        final View divider = new View(this);
        divider.setBackgroundColor(Color.rgb(45, 35, 45));
        final LinearLayout.LayoutParams divParams = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, dp(1));
        divParams.topMargin = dp(12);
        divParams.bottomMargin = dp(12);
        card.addView(divider, divParams);

        // Details inside card
        isoDetailText = makeDetailLine(card, "ISO Game", "Mencari file...");
        elfDetailText = makeDetailLine(card, "Boot ELF", "Belum diekstrak");
        logDetailText = makeDetailLine(card, "Log Sesi", "/sdcard/gow2/logs/gow2.log (Tunggal)");

        root.addView(card);

        // 3. Primary Action: Start Game Button
        startButton = new Button(this);
        startButton.setText("\u25B6  MULAI PERMAINAN");
        startButton.setTextSize(16.0f);
        startButton.setTypeface(Typeface.DEFAULT_BOLD);
        startButton.setTextColor(Color.WHITE);
        startButton.setPadding(dp(20), dp(16), dp(20), dp(16));

        final GradientDrawable playBg = new GradientDrawable();
        playBg.setColor(Color.rgb(196, 30, 58)); // Crimson Red
        playBg.setCornerRadius(dp(10));
        playBg.setStroke(dp(2), Color.rgb(225, 175, 60)); // Gold border
        startButton.setBackground(playBg);
        startButton.setOnClickListener(view -> startGame());

        final LinearLayout.LayoutParams startParams = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        startParams.topMargin = dp(20);
        root.addView(startButton, startParams);

        // 4. ISO Action Buttons Row
        final TextView optionsLabel = new TextView(this);
        optionsLabel.setText("PILIHAN SUMBER ISO GAME");
        optionsLabel.setTextColor(Color.rgb(175, 175, 190));
        optionsLabel.setTextSize(12.0f);
        optionsLabel.setTypeface(Typeface.DEFAULT_BOLD);
        optionsLabel.setPadding(0, dp(24), 0, dp(10));
        root.addView(optionsLabel);

        final LinearLayout buttonRow = new LinearLayout(this);
        buttonRow.setOrientation(LinearLayout.HORIZONTAL);

        addStyledButton(buttonRow, "\uD83D\uDCC1 Pilih ISO Langsung", view -> pickIsoDirect(), 1.0f);
        addStyledButton(buttonRow, "\uD83D\uDD0D Scan /sdcard/gow2", view -> scanSdcardGow2(), 1.0f);
        addStyledButton(buttonRow, "\uD83D\uDCE5 Impor (Salin)", view -> pickIsoImport(), 1.0f);

        root.addView(buttonRow);

        // 5. Utility Buttons (Settings & Storage Help)
        final LinearLayout utilRow = new LinearLayout(this);
        utilRow.setOrientation(LinearLayout.HORIZONTAL);
        final LinearLayout.LayoutParams utilParams = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        utilParams.topMargin = dp(8);

        addStyledButton(utilRow, "\u2699\uFE0F Pengaturan Grafis & Kontrol", view -> showSettings(), 1.0f);
        addStyledButton(utilRow, "\uD83D\uDEE1\uFE0F Izin Akses File", view -> checkAndRequestStoragePermissions(), 0.6f);

        root.addView(utilRow, utilParams);

        // Warning if native library failed to load
        if (!NativeBridge.isLoaded()) {
            final TextView warning = new TextView(this);
            warning.setText("\u26A0\uFE0F Library native tidak termuat:\n" + NativeBridge.loadError());
            warning.setTextColor(Color.rgb(255, 100, 100));
            warning.setTextSize(12.0f);
            warning.setPadding(0, dp(16), 0, 0);
            root.addView(warning);
        }

        scroll.addView(root);
        setContentView(scroll);
    }

    private TextView makeDetailLine(LinearLayout parent, String label, String value) {
        final LinearLayout line = new LinearLayout(this);
        line.setOrientation(LinearLayout.HORIZONTAL);
        line.setPadding(0, dp(3), 0, dp(3));

        final TextView lbl = new TextView(this);
        lbl.setText(label + ": ");
        lbl.setTextColor(Color.rgb(150, 150, 165));
        lbl.setTextSize(12.0f);
        lbl.setTypeface(Typeface.DEFAULT_BOLD);
        final LinearLayout.LayoutParams lpLbl = new LinearLayout.LayoutParams(dp(85), LinearLayout.LayoutParams.WRAP_CONTENT);
        line.addView(lbl, lpLbl);

        final TextView val = new TextView(this);
        val.setText(value);
        val.setTextColor(Color.rgb(230, 230, 240));
        val.setTextSize(12.0f);
        val.setTypeface(Typeface.MONOSPACE);
        line.addView(val);

        parent.addView(line);
        return val;
    }

    private void addStyledButton(LinearLayout parent, String label, View.OnClickListener listener, float weight) {
        final Button button = new Button(this);
        button.setText(label);
        button.setAllCaps(false);
        button.setTextSize(12.0f);
        button.setTextColor(Color.rgb(230, 230, 240));
        button.setPadding(dp(12), dp(10), dp(12), dp(10));

        final GradientDrawable bg = new GradientDrawable();
        bg.setColor(Color.rgb(32, 32, 44));
        bg.setCornerRadius(dp(8));
        bg.setStroke(dp(1), Color.rgb(70, 60, 75));
        button.setBackground(bg);
        button.setOnClickListener(listener);

        final LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(
                0, LinearLayout.LayoutParams.WRAP_CONTENT, weight);
        params.leftMargin = dp(4);
        params.rightMargin = dp(4);
        parent.addView(button, params);
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    // ---------------------------------------------------------------------
    // Status & ISO Resolution
    // ---------------------------------------------------------------------
    private void refreshStatus() {
        final File effectiveIso = SettingsStore.resolveEffectiveIso(this, settings);
        final File elf = SettingsStore.bootElfFile(this);
        final boolean isoReady = DiscInstaller.isReadable(effectiveIso);
        final boolean elfReady = DiscInstaller.isReadable(elf);

        // Update badge
        final GradientDrawable badgeBg = new GradientDrawable();
        badgeBg.setCornerRadius(dp(6));
        if (isoReady && elfReady) {
            statusBadge.setText("SIAP MAIN");
            statusBadge.setTextColor(Color.rgb(20, 40, 20));
            badgeBg.setColor(Color.rgb(46, 204, 113)); // Vibrant Green
        } else if (isoReady) {
            statusBadge.setText("BUTUH EKSTRAK ELF");
            statusBadge.setTextColor(Color.rgb(50, 40, 10));
            badgeBg.setColor(Color.rgb(241, 196, 15)); // Gold/Yellow
        } else {
            statusBadge.setText("ISO BELUM TERDETEKSI");
            statusBadge.setTextColor(Color.WHITE);
            badgeBg.setColor(Color.rgb(192, 57, 43)); // Crimson Red
        }
        statusBadge.setBackground(badgeBg);

        // Update detail strings
        if (effectiveIso != null && effectiveIso.exists()) {
            final String sourceLabel = effectiveIso.getAbsolutePath().startsWith("/sdcard/gow2")
                    ? "[/sdcard/gow2]"
                    : (effectiveIso.getAbsolutePath().equals(SettingsStore.isoFile(this).getAbsolutePath()) ? "[Imported]" : "[Direct]");
            isoDetailText.setText(effectiveIso.getName() + " (" + DiscInstaller.formatBytes(effectiveIso.length()) + ") " + sourceLabel);
        } else {
            isoDetailText.setText("Tidak ada ISO aktif (Pilih atau taruh di /sdcard/gow2)");
        }

        elfDetailText.setText(elfReady ? elf.getName() + " (Terverifikasi)" : "Belum diekstrak");
        logDetailText.setText("/sdcard/gow2/logs/gow2.log (Satu file per sesi)");

        startButton.setEnabled(isoReady && !busy && NativeBridge.isLoaded());
    }

    private void autoDetectIsoIfNeeded() {
        if (settings.isoPath == null || settings.isoPath.isEmpty()) {
            File sdcardIso = SettingsStore.findSdcardIso();
            if (sdcardIso != null) {
                settings.isoPath = sdcardIso.getAbsolutePath();
                settings.isoSource = "sdcard";
                store.save(settings);
                Log.i(TAG, "Auto-detected ISO in /sdcard/gow2: " + sdcardIso.getAbsolutePath());
            }
        }
    }

    // ---------------------------------------------------------------------
    // Game Launch & Boot Data Extraction
    // ---------------------------------------------------------------------
    private void startGame() {
        final File effectiveIso = SettingsStore.resolveEffectiveIso(this, settings);
        if (!DiscInstaller.isReadable(effectiveIso)) {
            Toast.makeText(this, "Pilih ISO God of War II atau taruh di /sdcard/gow2", Toast.LENGTH_SHORT).show();
            return;
        }

        final File elf = SettingsStore.bootElfFile(this);
        if (DiscInstaller.isReadable(elf)) {
            launchGame();
            return;
        }

        // Auto-extract ELF from the effective ISO
        extractBootData(effectiveIso);
    }

    private void launchGame() {
        settings = store.get();
        // Ensure active isoPath and bootElf are saved
        File effectiveIso = SettingsStore.resolveEffectiveIso(this, settings);
        if (effectiveIso != null) {
            settings.isoPath = effectiveIso.getAbsolutePath();
        }
        File effectiveElf = SettingsStore.resolveEffectiveElf(this);
        if (effectiveElf != null && effectiveElf.exists()) {
            settings.bootElf = effectiveElf.getAbsolutePath();
        }
        store.save(settings);
        Log.i(TAG, "Launching game: ISO=" + settings.isoPath + " ELF=" + settings.bootElf);
        startActivity(GameActivity.createIntent(this, settings));
    }

    private void extractBootData(File iso) {
        if (!NativeBridge.isLoaded()) {
            Toast.makeText(this, "Library native tidak termuat", Toast.LENGTH_LONG).show();
            return;
        }
        setBusy(true, "Mengekstrak boot ELF SCUS_974.81 dari ISO...");
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
                        ? "Boot ELF " + object.optString("elf", "") + " (SHA-1 cocok: " + object.optBoolean("match", false) + ")"
                        : object.optString("error", "Gagal mengekstrak ELF");
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
                            .setTitle("Ekstraksi Boot ELF Gagal")
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
            Toast.makeText(this, message, Toast.LENGTH_SHORT).show();
        }
        final File effectiveIso = SettingsStore.resolveEffectiveIso(this, settings);
        startButton.setEnabled(!value && DiscInstaller.isReadable(effectiveIso));
    }

    // ---------------------------------------------------------------------
    // ISO Selection Actions
    // ---------------------------------------------------------------------
    private void pickIsoDirect() {
        final Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        intent.putExtra(Intent.EXTRA_MIME_TYPES, new String[]{"application/x-iso9660-image", "application/octet-stream"});
        try {
            startActivityForResult(intent, REQUEST_ISO_DIRECT);
        } catch (RuntimeException error) {
            Log.e(TAG, "No document picker available", error);
            Toast.makeText(this, "Aplikasi pemilih berkas tidak ditemukan", Toast.LENGTH_LONG).show();
        }
    }

    private void pickIsoImport() {
        final Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        intent.putExtra(Intent.EXTRA_MIME_TYPES, new String[]{"application/x-iso9660-image", "application/octet-stream"});
        try {
            startActivityForResult(intent, REQUEST_ISO_IMPORT);
        } catch (RuntimeException error) {
            Log.e(TAG, "No document picker available", error);
            Toast.makeText(this, "Aplikasi pemilih berkas tidak ditemukan", Toast.LENGTH_LONG).show();
        }
    }

    private void scanSdcardGow2() {
        File sdcardIso = SettingsStore.findSdcardIso();
        if (sdcardIso != null) {
            settings.isoPath = sdcardIso.getAbsolutePath();
            settings.isoSource = "sdcard";
            store.save(settings);
            refreshStatus();
            Toast.makeText(this, "Ditemukan di /sdcard/gow2: " + sdcardIso.getName(), Toast.LENGTH_LONG).show();
            // Check if ELF needs extraction
            if (!DiscInstaller.isReadable(SettingsStore.bootElfFile(this))) {
                extractBootData(sdcardIso);
            }
        } else {
            new AlertDialog.Builder(this)
                    .setTitle("ISO Tidak Ditemukan di /sdcard/gow2")
                    .setMessage("Pastikan file ISO ditaruh di folder /sdcard/gow2 (contoh: /sdcard/gow2/gow2.iso atau God of War II.iso) lalu tekan scan kembali.")
                    .setPositiveButton("OK", null)
                    .show();
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (resultCode != RESULT_OK || data == null || data.getData() == null) {
            return;
        }

        final Uri uri = data.getData();
        try {
            getContentResolver().takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (SecurityException error) {
            Log.w(TAG, "Cannot persist URI permission", error);
        }

        if (requestCode == REQUEST_ISO_DIRECT) {
            handleDirectIsoPick(uri);
        } else if (requestCode == REQUEST_ISO_IMPORT) {
            importIso(uri);
        }
    }

    private void handleDirectIsoPick(Uri uri) {
        String directPath = DiscInstaller.resolvePathFromUri(this, uri);
        if (directPath != null && new File(directPath).canRead()) {
            settings.isoPath = directPath;
            settings.isoSource = "direct";
            settings.isoUri = uri.toString();
            store.save(settings);
            refreshStatus();
            Toast.makeText(this, "ISO dipilih langsung: " + new File(directPath).getName(), Toast.LENGTH_SHORT).show();
            extractBootData(new File(directPath));
        } else {
            // If cannot resolve direct path, ask user whether to import (copy)
            new AlertDialog.Builder(this)
                    .setTitle("Penyimpanan Eksternal")
                    .setMessage("File tidak dapat dibaca langsung melalui path. Salin (impor) file ini ke memori internal aplikasi?")
                    .setNegativeButton("Batal", null)
                    .setPositiveButton("Salin Sekarang", (dialog, which) -> importIso(uri))
                    .show();
        }
    }

    private void importIso(Uri uri) {
        final File destination = SettingsStore.isoFile(this);
        setBusy(true, "Menyalin ISO...");

        final AlertDialog progress = new AlertDialog.Builder(this)
                .setTitle("Mengimpor ISO God of War II")
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
                    settings.isoPath = destination.getAbsolutePath();
                    settings.isoSource = "imported";
                    store.save(settings);
                    refreshStatus();
                    extractBootData(destination);
                } else {
                    new AlertDialog.Builder(this)
                            .setTitle("Impor Gagal")
                            .setMessage(result.error)
                            .setPositiveButton("OK", null)
                            .show();
                }
            });
        });
    }

    // ---------------------------------------------------------------------
    // Settings Dialog
    // ---------------------------------------------------------------------
    private void showSettings() {
        final com.gow2.android.Settings draft = store.get();
        final LinearLayout content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setPadding(dp(20), dp(12), dp(20), dp(12));

        final RadioGroup presetGroup = new RadioGroup(this);
        presetGroup.setOrientation(RadioGroup.HORIZONTAL);
        addRadio(presetGroup, "Performa", com.gow2.android.Settings.PRESET_PERFORMANCE);
        addRadio(presetGroup, "Seimbang", com.gow2.android.Settings.PRESET_BALANCED);
        addRadio(presetGroup, "Kualitas", com.gow2.android.Settings.PRESET_QUALITY);
        checkRadio(presetGroup, draft.preset);
        content.addView(label("Preset Grafik"));
        content.addView(presetGroup);

        final RadioGroup aspectGroup = new RadioGroup(this);
        aspectGroup.setOrientation(RadioGroup.HORIZONTAL);
        addRadio(aspectGroup, "Penuh", com.gow2.android.Settings.ASPECT_FILL);
        addRadio(aspectGroup, "4:3", com.gow2.android.Settings.ASPECT_4X3);
        addRadio(aspectGroup, "Fit", com.gow2.android.Settings.ASPECT_FIT);
        checkRadio(aspectGroup, draft.aspect);
        content.addView(label("Aspek Layar"));
        content.addView(aspectGroup);

        final CheckBox vsync = new CheckBox(this);
        vsync.setText("VSync (Sinkronisasi Vertikal)");
        vsync.setTextColor(Color.WHITE);
        vsync.setChecked(draft.vsync);
        content.addView(vsync);

        final SeekBar volume = addSlider(content, "Volume Suara", 0, 100, Math.round(draft.volume * 100.0f));
        final SeekBar touchScale = addSlider(content, "Ukuran Tombol Sentuh", 70, 160, Math.round(draft.touchScale * 100.0f));
        final SeekBar touchOpacity = addSlider(content, "Opasitas Tombol Sentuh", 15, 100, Math.round(draft.touchOpacity * 100.0f));
        final SeekBar deadzone = addSlider(content, "Deadzone Analog", 0, 45, Math.round(draft.deadzone * 100.0f));

        final CheckBox leftHanded = new CheckBox(this);
        leftHanded.setText("Tata Letak Tombol Kidal");
        leftHanded.setTextColor(Color.WHITE);
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
        content.addView(label("Tingkat Log Debug"));
        content.addView(logLevel);

        final ScrollView scroll = new ScrollView(this);
        scroll.addView(content);

        new AlertDialog.Builder(this)
                .setTitle("Pengaturan Game")
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
        view.setTextColor(Color.rgb(200, 180, 140));
        view.setTextSize(12.0f);
        view.setTypeface(Typeface.DEFAULT_BOLD);
        view.setPadding(0, dp(14), 0, dp(3));
        return view;
    }

    private void addRadio(RadioGroup group, String text, String value) {
        final RadioButton button = new RadioButton(this);
        button.setText(text);
        button.setTextColor(Color.WHITE);
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
