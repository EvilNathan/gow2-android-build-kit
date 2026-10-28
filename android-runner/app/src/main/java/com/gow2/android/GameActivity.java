package com.gow2.android;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.Intent;
import android.content.res.Configuration;
import android.graphics.Color;
import android.graphics.Typeface;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.os.VibratorManager;
import android.util.Log;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.TextView;
import android.widget.Toast;

/**
 * Game screen: a {@link SurfaceView} whose surface is handed to the native host (and from there to the
 * Vulkan GS backend), with the {@link TouchControlsView} overlay on top and a status HUD toggled by a
 * three-finger tap.
 *
 * <p>The engine runs on its own native thread; this activity never blocks on it.  Pausing the
 * activity parks that thread through {@code nativeSetPaused(true)} and resuming releases it, so the
 * session survives HOME/recents and screen lock.
 */
public final class GameActivity extends Activity implements NativeBridge.RumbleSink {

    public static final String EXTRA_SETTINGS_JSON = "com.gow2.android.extra.SETTINGS_JSON";

    private static final String TAG = "PS2X";
    private static final long HUD_REFRESH_MS = 500L;
    private static final long THREE_FINGER_TAP_MS = 600L;

    private FrameLayout root;
    private SurfaceView surfaceView;
    private TouchControlsView touchControls;
    private TextView hud;

    private PadMapper padMapper;
    private Settings settings;
    private final Handler handler = new Handler(Looper.getMainLooper());

    private Vibrator vibrator;

    private boolean nativeStarted;
    private boolean surfaceReady;
    private boolean resumed;
    private boolean hudVisible;
    private boolean exitDialogShowing;

    private final float[] tapStart = new float[2];
    private boolean threeFingerCandidate;
    private boolean threeFingerMoved;
    private long threeFingerStartMs;

    private final Runnable hudTick = new Runnable() {
        @Override
        public void run() {
            if (!hudVisible) {
                return;
            }
            updateHud();
            handler.postDelayed(this, HUD_REFRESH_MS);
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        settings = Settings.fromJson(getIntent().getStringExtra(EXTRA_SETTINGS_JSON));
        padMapper = new PadMapper(new PadMapper.Sink() {
            @Override
            public void onPadState(int buttons, int lx, int ly, int rx, int ry) {
                if (nativeStarted && NativeBridge.isLoaded()) {
                    NativeBridge.nativeSetPadState(buttons, lx, ly, rx, ry);
                }
            }
        });
        padMapper.setDeadzone(settings.deadzone);

        buildUi();
        applyImmersiveMode();
        initVibrator();

        if (!NativeBridge.isLoaded()) {
            Toast.makeText(this, "Library native tidak termuat: " + NativeBridge.loadError(), Toast.LENGTH_LONG).show();
            Log.e(TAG, "native library unavailable: " + NativeBridge.loadError());
            finish();
            return;
        }

        NativeBridge.setRumbleSink(this);
        final String filesDir = SettingsStore.filesPath(this);
        NativeBridge.nativeStart(filesDir, settings.toJson());
        NativeBridge.nativeSetPaused(true); // released once the surface exists and we are resumed
        nativeStarted = true;
    }

    private void buildUi() {
        root = new FrameLayout(this);
        root.setBackgroundColor(Color.BLACK);

        surfaceView = new SurfaceView(this);
        surfaceView.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override
            public void surfaceCreated(SurfaceHolder holder) {
                attachSurface(holder.getSurface());
            }

            @Override
            public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
                NativeBridge.nativeSurfaceChanged(width, height);
                attachSurface(holder.getSurface());
            }

            @Override
            public void surfaceDestroyed(SurfaceHolder holder) {
                surfaceReady = false;
                NativeBridge.nativeSetSurface(null);
                NativeBridge.nativeSetPaused(true);
            }
        });
        root.addView(surfaceView, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));

        touchControls = new TouchControlsView(this);
        touchControls.configure(settings);
        touchControls.setPadSink(new TouchControlsView.PadSink() {
            @Override
            public void onPadState(int buttons, int lx, int ly, int rx, int ry) {
                padMapper.setTouchState(buttons, lx, ly, rx, ry);
            }
        });
        root.addView(touchControls, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));

        hud = new TextView(this);
        hud.setTextColor(Color.WHITE);
        hud.setTextSize(11.0f);
        hud.setTypeface(Typeface.MONOSPACE);
        hud.setBackgroundColor(Color.argb(150, 0, 0, 0));
        hud.setPadding(dp(8), dp(4), dp(8), dp(4));
        hud.setVisibility(View.GONE);
        final FrameLayout.LayoutParams hudParams = new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT);
        hudParams.gravity = Gravity.TOP | Gravity.START;
        hudParams.setMargins(dp(10), dp(10), 0, 0);
        root.addView(hud, hudParams);

        setContentView(root);
    }

    private void attachSurface(Surface surface) {
        if (surface == null || !surface.isValid()) {
            return;
        }
        NativeBridge.nativeSetSurface(surface);
        surfaceReady = true;
        updatePauseState();
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    // ---------------------------------------------------------------------
    // Lifecycle
    // ---------------------------------------------------------------------
    @Override
    protected void onResume() {
        super.onResume();
        resumed = true;
        applyImmersiveMode();
        updatePauseState();
    }

    @Override
    protected void onPause() {
        resumed = false;
        if (nativeStarted) {
            NativeBridge.nativeSetPaused(true);
        }
        // Never leave a button latched in the engine while we are in the background.
        padMapper.reset();
        touchControls.resetState();
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        handler.removeCallbacks(hudTick);
        NativeBridge.setRumbleSink(null);
        if (isFinishing() && nativeStarted) {
            NativeBridge.nativeRequestExit();
            NativeBridge.nativeStop();
            nativeStarted = false;
        }
        super.onDestroy();
    }

    @Override
    public void onConfigurationChanged(Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
        applyImmersiveMode();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            applyImmersiveMode();
        }
    }

    private void updatePauseState() {
        if (!nativeStarted) {
            return;
        }
        final boolean running = resumed && surfaceReady && !exitDialogShowing;
        NativeBridge.nativeSetPaused(!running);
        if (running) {
            padMapper.reset();
        }
    }

    private void applyImmersiveMode() {
        final Window window = getWindow();
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON
                | WindowManager.LayoutParams.FLAG_FULLSCREEN);
        final View decor = window.getDecorView();
        decor.setSystemUiVisibility(View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_FULLSCREEN);
    }

    // ---------------------------------------------------------------------
    // Input
    // ---------------------------------------------------------------------
    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (event.getKeyCode() == KeyEvent.KEYCODE_BACK) {
            return super.dispatchKeyEvent(event);
        }
        if (padMapper.handleKeyEvent(event)) {
            return true;
        }
        return super.dispatchKeyEvent(event);
    }

    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent event) {
        if (padMapper.handleMotionEvent(event)) {
            return true;
        }
        return super.dispatchGenericMotionEvent(event);
    }

    @Override
    public boolean dispatchTouchEvent(MotionEvent event) {
        trackThreeFingerTap(event);
        return super.dispatchTouchEvent(event);
    }

    @Override
    public void onBackPressed() {
        if (exitDialogShowing) {
            return;
        }
        padMapper.reset();
        touchControls.resetState();
        if (nativeStarted) {
            NativeBridge.nativeSetPaused(true);
        }
        showExitDialog();
    }

    private void showExitDialog() {
        exitDialogShowing = true;
        new AlertDialog.Builder(this)
                .setTitle("Jeda permainan")
                .setMessage("Keluar dari God of War II? Progres yang belum disimpan bisa hilang.")
                .setNegativeButton("Lanjutkan", (dialog, which) -> {
                    exitDialogShowing = false;
                    updatePauseState();
                })
                .setPositiveButton("Keluar", (dialog, which) -> {
                    exitDialogShowing = false;
                    if (nativeStarted) {
                        NativeBridge.nativeRequestExit();
                    }
                    finish();
                })
                .setOnCancelListener(dialog -> {
                    exitDialogShowing = false;
                    updatePauseState();
                })
                .show();
    }

    /** Three-finger tap toggles the status HUD; the tap is swallowed so it cannot press a control. */
    private void trackThreeFingerTap(MotionEvent event) {
        final int action = event.getActionMasked();
        if (action == MotionEvent.ACTION_POINTER_DOWN && event.getPointerCount() == 3) {
            threeFingerCandidate = true;
            threeFingerMoved = false;
            threeFingerStartMs = SystemClock.uptimeMillis();
            tapStart[0] = centroid(event, true);
            tapStart[1] = centroid(event, false);
            return;
        }
        if (!threeFingerCandidate) {
            return;
        }
        if (action == MotionEvent.ACTION_MOVE) {
            if (event.getPointerCount() < 3) {
                threeFingerCandidate = false;
                return;
            }
            final float dx = centroid(event, true) - tapStart[0];
            final float dy = centroid(event, false) - tapStart[1];
            if (Math.hypot(dx, dy) > dp(48)) {
                threeFingerMoved = true;
            }
            return;
        }
        if (action == MotionEvent.ACTION_POINTER_UP || action == MotionEvent.ACTION_UP
                || action == MotionEvent.ACTION_CANCEL) {
            final boolean tapped = !threeFingerMoved && event.getPointerCount() >= 3
                    && SystemClock.uptimeMillis() - threeFingerStartMs <= THREE_FINGER_TAP_MS;
            threeFingerCandidate = false;
            if (tapped) {
                touchControls.resetState();
                padMapper.clearTouchState();
                toggleHud();
            }
        }
    }

    private static float centroid(MotionEvent event, boolean xAxis) {
        float sum = 0.0f;
        for (int i = 0; i < event.getPointerCount(); i++) {
            sum += xAxis ? event.getX(i) : event.getY(i);
        }
        return event.getPointerCount() > 0 ? sum / event.getPointerCount() : 0.0f;
    }

    private void toggleHud() {
        hudVisible = !hudVisible;
        hud.setVisibility(hudVisible ? View.VISIBLE : View.GONE);
        handler.removeCallbacks(hudTick);
        if (hudVisible) {
            updateHud();
            handler.postDelayed(hudTick, HUD_REFRESH_MS);
        }
    }

    private void updateHud() {
        String stats = "";
        try {
            stats = NativeBridge.nativeHostStats();
        } catch (RuntimeException error) {
            Log.w(TAG, "host stats unavailable", error);
        }
        hud.setText("GOW2 | " + stats);
    }

    // ---------------------------------------------------------------------
    // Rumble
    // ---------------------------------------------------------------------
    private void initVibrator() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            final VibratorManager manager = (VibratorManager) getSystemService(Context.VIBRATOR_MANAGER_SERVICE);
            if (manager != null) {
                vibrator = manager.getDefaultVibrator();
            }
        }
        if (vibrator == null) {
            vibrator = (Vibrator) getSystemService(Context.VIBRATOR_SERVICE);
        }
    }

    @Override
    public void onRumble(float low, float high) {
        final Vibrator device = vibrator;
        if (device == null || !device.hasVibrator()) {
            return;
        }
        final float strength = Math.max(low, high);
        if (strength < 0.05f) {
            device.cancel();
            return;
        }
        final int amplitude = Math.max(1, Math.min(255, Math.round(strength * 255.0f)));
        final int durationMs = low > 0.0f && high <= 0.0f ? 40 : 120;
        try {
            device.vibrate(VibrationEffect.createOneShot(durationMs, amplitude));
        } catch (RuntimeException error) {
            Log.w(TAG, "vibrate failed", error);
        }
    }

    /** Convenience for MainActivity: build the intent that launches the game screen. */
    public static Intent createIntent(Context context, Settings settings) {
        final Intent intent = new Intent(context, GameActivity.class);
        intent.putExtra(EXTRA_SETTINGS_JSON, settings.toJson());
        intent.addFlags(Intent.FLAG_ACTIVITY_NO_ANIMATION);
        return intent;
    }
}
