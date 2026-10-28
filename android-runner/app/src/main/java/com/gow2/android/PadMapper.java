package com.gow2.android;

import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;

/**
 * Maps Android key/gamepad events and touch-overlay state onto the PS2 pad bitfield.
 *
 * <p>Bit values are ACTIVE-HIGH and match {@code ps2x::host::PAD_*} in
 * android-runner/native/host/ps2x_host.h (PS2 DualShock serial layout).  The native side inverts
 * them for the engine.
 *
 * <p>Two input sources (touch overlay and physical gamepad) are merged before publishing: buttons
 * are OR-ed, and per axis the value with the larger magnitude wins.
 */
public final class PadMapper {

    public static final int PAD_SELECT = 0x0001;
    public static final int PAD_L3 = 0x0002;
    public static final int PAD_R3 = 0x0004;
    public static final int PAD_START = 0x0008;
    public static final int PAD_UP = 0x0010;
    public static final int PAD_RIGHT = 0x0020;
    public static final int PAD_DOWN = 0x0040;
    public static final int PAD_LEFT = 0x0080;
    public static final int PAD_L2 = 0x0100;
    public static final int PAD_R2 = 0x0200;
    public static final int PAD_L1 = 0x0400;
    public static final int PAD_R1 = 0x0800;
    public static final int PAD_TRIANGLE = 0x1000;
    public static final int PAD_CIRCLE = 0x2000;
    public static final int PAD_CROSS = 0x4000;
    public static final int PAD_SQUARE = 0x8000;

    /** Axis range used by the host API. */
    public static final int AXIS_MAX = 127;
    public static final int AXIS_MIN = -128;

    private static final float TRIGGER_THRESHOLD = 0.5f;
    private static final float HAT_THRESHOLD = 0.5f;

    public interface Sink {
        void onPadState(int buttons, int lx, int ly, int rx, int ry);
    }

    private final Sink sink;

    private volatile float deadzone = 0.18f;

    private int padButtons;
    private int padLx;
    private int padLy;
    private int padRx;
    private int padRy;

    private int touchButtons;
    private int touchLx;
    private int touchLy;
    private int touchRx;
    private int touchRy;

    public PadMapper(Sink sink) {
        this.sink = sink;
    }

    public void setDeadzone(float deadzone) {
        this.deadzone = Settings.clamp(deadzone, 0.0f, 0.45f);
    }

    public float deadzone() {
        return deadzone;
    }

    /** Called by the touch overlay whenever its own state changes. */
    public void setTouchState(int buttons, int lx, int ly, int rx, int ry) {
        touchButtons = buttons & 0xFFFF;
        touchLx = clampAxis(lx);
        touchLy = clampAxis(ly);
        touchRx = clampAxis(rx);
        touchRy = clampAxis(ry);
        publish();
    }

    public void clearTouchState() {
        setTouchState(0, 0, 0, 0, 0);
    }

    /** @return true when the key belongs to the pad and was consumed. */
    public boolean handleKeyEvent(KeyEvent event) {
        if (event == null) {
            return false;
        }
        final int bit = bitForKeys(event.getKeyCode());
        if (bit == 0) {
            return false;
        }
        final boolean down = event.getAction() == KeyEvent.ACTION_DOWN;
        if (down) {
            padButtons |= bit;
        } else {
            padButtons &= ~bit;
        }
        publish();
        return true;
    }

    /** @return true when the event came from a gamepad/joystick and was consumed. */
    public boolean handleMotionEvent(MotionEvent event) {
        if (event == null) {
            return false;
        }
        final int source = event.getSource();
        final boolean joystick = (source & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK
                || (source & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD;
        if (!joystick) {
            return false;
        }

        final float leftX = axis(event, MotionEvent.AXIS_X);
        final float leftY = axis(event, MotionEvent.AXIS_Y);
        float rightX = axis(event, MotionEvent.AXIS_Z);
        float rightY = axis(event, MotionEvent.AXIS_RZ);
        if (rightX == 0.0f && rightY == 0.0f) {
            // Some pads report the right stick on RX/RY instead of Z/RZ.
            rightX = axis(event, MotionEvent.AXIS_RX);
            rightY = axis(event, MotionEvent.AXIS_RY);
        }

        padLx = toPadAxis(leftX, deadzone);
        padLy = toPadAxis(leftY, deadzone);
        padRx = toPadAxis(rightX, deadzone);
        padRy = toPadAxis(rightY, deadzone);

        final float hatX = axis(event, MotionEvent.AXIS_HAT_X);
        final float hatY = axis(event, MotionEvent.AXIS_HAT_Y);
        setButton(PAD_LEFT, hatX < -HAT_THRESHOLD);
        setButton(PAD_RIGHT, hatX > HAT_THRESHOLD);
        setButton(PAD_UP, hatY < -HAT_THRESHOLD);
        setButton(PAD_DOWN, hatY > HAT_THRESHOLD);

        final float l2 = Math.max(axis(event, MotionEvent.AXIS_BRAKE), axis(event, MotionEvent.AXIS_LTRIGGER));
        final float r2 = Math.max(axis(event, MotionEvent.AXIS_GAS), axis(event, MotionEvent.AXIS_RTRIGGER));
        setButton(PAD_L2, l2 > TRIGGER_THRESHOLD);
        setButton(PAD_R2, r2 > TRIGGER_THRESHOLD);

        publish();
        return true;
    }

    /** Clears everything (activity pause/teardown) so no button stays stuck. */
    public void reset() {
        padButtons = 0;
        padLx = 0;
        padLy = 0;
        padRx = 0;
        padRy = 0;
        touchButtons = 0;
        touchLx = 0;
        touchLy = 0;
        touchRx = 0;
        touchRy = 0;
        publish();
    }

    private void publish() {
        final int buttons = (padButtons | touchButtons) & 0xFFFF;
        final int lx = pickByMagnitude(touchLx, padLx);
        final int ly = pickByMagnitude(touchLy, padLy);
        final int rx = pickByMagnitude(touchRx, padRx);
        final int ry = pickByMagnitude(touchRy, padRy);
        sink.onPadState(buttons, lx, ly, rx, ry);
    }

    private void setButton(int bit, boolean pressed) {
        if (pressed) {
            padButtons |= bit;
        } else {
            padButtons &= ~bit;
        }
    }

    private static int pickByMagnitude(int touch, int pad) {
        return Math.abs(touch) >= Math.abs(pad) ? touch : pad;
    }

    private static int bitForKeys(int keyCode) {
        switch (keyCode) {
            case KeyEvent.KEYCODE_BUTTON_A:
                return PAD_CROSS;
            case KeyEvent.KEYCODE_BUTTON_B:
                return PAD_CIRCLE;
            case KeyEvent.KEYCODE_BUTTON_X:
                return PAD_SQUARE;
            case KeyEvent.KEYCODE_BUTTON_Y:
                return PAD_TRIANGLE;
            case KeyEvent.KEYCODE_BUTTON_Z:
                return PAD_L2;
            case KeyEvent.KEYCODE_BUTTON_L1:
                return PAD_L1;
            case KeyEvent.KEYCODE_BUTTON_R1:
                return PAD_R1;
            case KeyEvent.KEYCODE_BUTTON_L2:
                return PAD_L2;
            case KeyEvent.KEYCODE_BUTTON_R2:
                return PAD_R2;
            case KeyEvent.KEYCODE_BUTTON_THUMBL:
                return PAD_L3;
            case KeyEvent.KEYCODE_BUTTON_THUMBR:
                return PAD_R3;
            case KeyEvent.KEYCODE_BUTTON_START:
            case KeyEvent.KEYCODE_MENU:
                return PAD_START;
            case KeyEvent.KEYCODE_BUTTON_SELECT:
                return PAD_SELECT;
            case KeyEvent.KEYCODE_DPAD_UP:
                return PAD_UP;
            case KeyEvent.KEYCODE_DPAD_DOWN:
                return PAD_DOWN;
            case KeyEvent.KEYCODE_DPAD_LEFT:
                return PAD_LEFT;
            case KeyEvent.KEYCODE_DPAD_RIGHT:
                return PAD_RIGHT;
            default:
                return 0;
        }
    }

    /** Reads an axis, tolerating devices that do not expose it (getAxisValue throws then). */
    private static float axis(MotionEvent event, int axis) {
        try {
            return event.getAxisValue(axis);
        } catch (IllegalArgumentException ignored) {
            return 0.0f;
        }
    }

    /** Maps a -1..1 axis onto the host's -128..127 range, applying the deadzone and rescaling. */
    public static int toPadAxis(float value, float deadzone) {
        float clamped = Settings.clamp(value, -1.0f, 1.0f);
        final float magnitude = Math.abs(clamped);
        final float zone = Settings.clamp(deadzone, 0.0f, 0.95f);
        if (magnitude <= zone) {
            return 0;
        }
        float scaled = (magnitude - zone) / (1.0f - zone);
        if (clamped < 0.0f) {
            scaled = -scaled;
        }
        final int mapped = Math.round(scaled * AXIS_MAX);
        return clampAxis(mapped);
    }

    public static int clampAxis(int value) {
        return Math.max(AXIS_MIN, Math.min(AXIS_MAX, value));
    }
}
