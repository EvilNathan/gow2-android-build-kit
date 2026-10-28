package com.gow2.android;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Typeface;
import android.util.SparseIntArray;
import android.view.HapticFeedbackConstants;
import android.view.MotionEvent;
import android.view.View;

import java.util.ArrayList;
import java.util.List;

/**
 * Multi-touch gamepad overlay: left analog stick, D-pad, face buttons, shoulders and start/select.
 *
 * <p>The overlay only consumes touches that start on one of its controls; a touch on empty space is
 * returned unhandled so surrounding views still see it.  Every state change is published through
 * {@link PadSink#onPadState} which forwards to {@code NativeBridge.nativeSetPadState}.
 *
 * <p>All geometry is derived from the view size and the configured scale, so it adapts to any
 * landscape resolution without XML resources.
 */
public final class TouchControlsView extends View {

    public interface PadSink {
        void onPadState(int buttons, int lx, int ly, int rx, int ry);
    }

    // Control identifiers (also indexes into the zone table).
    private static final int STICK = 0;
    private static final int DPAD_UP = 1;
    private static final int DPAD_DOWN = 2;
    private static final int DPAD_LEFT = 3;
    private static final int DPAD_RIGHT = 4;
    private static final int TRIANGLE = 5;
    private static final int CIRCLE = 6;
    private static final int CROSS = 7;
    private static final int SQUARE = 8;
    private static final int L1 = 9;
    private static final int R1 = 10;
    private static final int L2 = 11;
    private static final int R2 = 12;
    private static final int START = 13;
    private static final int SELECT = 14;
    private static final int L3 = 15;
    private static final int R3 = 16;
    private static final int CONTROL_COUNT = 17;
    private static final int NONE = -1;

    /** A single circular control on screen. */
    private static final class Zone {
        final int id;
        final float cx;
        final float cy;
        final float radius;

        Zone(int id, float cx, float cy, float radius) {
            this.id = id;
            this.cx = cx;
            this.cy = cy;
            this.radius = radius;
        }

        boolean contains(float x, float y, float slack) {
            final float dx = x - cx;
            final float dy = y - cy;
            final float limit = radius * slack;
            return dx * dx + dy * dy <= limit * limit;
        }
    }

    private final float density;

    private final Paint fillPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint strokePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint textPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint accentPaint = new Paint(Paint.ANTI_ALIAS_FLAG);

    private final List<Zone> zones = new ArrayList<>(CONTROL_COUNT);
    private final boolean[] pressed = new boolean[CONTROL_COUNT];
    private final SparseIntArray pointerControl = new SparseIntArray(8);

    private PadSink sink;
    private float scale = 1.0f;
    private float opacity = 0.6f;
    private boolean leftHanded;
    private boolean haptics = true;
    private boolean stickVisible = true;
    private boolean dpadVisible = true;

    private int stickPointer = NONE;
    private float stickX;
    private float stickY;
    private float stickRadius;
    private int axisX;
    private int axisY;

    private float lastX;
    private float lastY;

    public TouchControlsView(Context context) {
        super(context);
        this.density = getResources().getDisplayMetrics().density;
        setFocusable(false);
        setClickable(false);
        setWillNotDraw(false);

        fillPaint.setStyle(Paint.Style.FILL);
        strokePaint.setStyle(Paint.Style.STROKE);
        strokePaint.setStrokeWidth(1.5f * density);
        textPaint.setTextAlign(Paint.Align.CENTER);
        textPaint.setTypeface(Typeface.create(Typeface.DEFAULT, Typeface.BOLD));
        accentPaint.setStyle(Paint.Style.FILL);
        accentPaint.setColor(Color.rgb(255, 112, 76));
    }

    public void setPadSink(PadSink sink) {
        this.sink = sink;
    }

    public void setScale(float scale) {
        this.scale = Settings.clamp(scale, 0.7f, 1.6f);
        rebuildZones();
    }

    public void setOpacity(float opacity) {
        this.opacity = Settings.clamp(opacity, 0.15f, 1.0f);
        invalidate();
    }

    public void setLeftHanded(boolean leftHanded) {
        this.leftHanded = leftHanded;
        rebuildZones();
    }

    public void setHapticsEnabled(boolean enabled) {
        this.haptics = enabled;
    }

    public void setStickVisible(boolean visible) {
        this.stickVisible = visible;
        rebuildZones();
    }

    public void setDpadVisible(boolean visible) {
        this.dpadVisible = visible;
        rebuildZones();
    }

    /** Applies every layout/feel setting in one call. */
    public void configure(Settings settings) {
        if (settings == null) {
            return;
        }
        this.scale = Settings.clamp(settings.touchScale, 0.7f, 1.6f);
        this.opacity = Settings.clamp(settings.touchOpacity, 0.15f, 1.0f);
        this.leftHanded = settings.leftHanded;
        rebuildZones();
    }

    // ---------------------------------------------------------------------
    // Geometry
    // ---------------------------------------------------------------------
    @Override
    protected void onSizeChanged(int width, int height, int oldWidth, int oldHeight) {
        super.onSizeChanged(width, height, oldWidth, oldHeight);
        rebuildZones();
    }

    private void rebuildZones() {
        zones.clear();
        final int width = getWidth();
        final int height = getHeight();
        if (width <= 0 || height <= 0) {
            return;
        }

        final float unit = Math.min(width, height);
        final float faceRadius = unit * 0.058f * scale;
        final float faceOffset = faceRadius * 1.6f;
        final float dpadRadius = faceRadius * 0.9f;
        final float dpadOffset = dpadRadius * 1.45f;
        final float smallRadius = faceRadius * 0.78f;
        final float margin = 8.0f * density;

        final float faceX = leftHanded ? (margin + faceRadius * 2.6f) : (width - margin - faceRadius * 2.6f);
        final float faceY = height * 0.76f;

        final float dpadX = leftHanded ? (width - margin - dpadRadius * 4.2f) : (margin + dpadRadius * 4.2f);
        final float dpadY = height * 0.78f;

        final float stickBaseX = leftHanded ? (width - margin - unit * 0.14f) : (margin + unit * 0.14f);
        final float stickBaseY = height * 0.75f;
        stickX = stickBaseX;
        stickY = stickBaseY;
        stickRadius = unit * 0.135f * scale;

        if (stickVisible) {
            zones.add(new Zone(STICK, stickX, stickY, stickRadius));
        }
        if (dpadVisible) {
            zones.add(new Zone(DPAD_UP, dpadX, dpadY - dpadOffset, dpadRadius));
            zones.add(new Zone(DPAD_DOWN, dpadX, dpadY + dpadOffset, dpadRadius));
            zones.add(new Zone(DPAD_LEFT, dpadX - dpadOffset, dpadY, dpadRadius));
            zones.add(new Zone(DPAD_RIGHT, dpadX + dpadOffset, dpadY, dpadRadius));
        }

        // Face diamond: triangle up, circle right, cross down, square left.
        zones.add(new Zone(TRIANGLE, faceX, faceY - faceOffset, faceRadius));
        zones.add(new Zone(CIRCLE, faceX + faceOffset, faceY, faceRadius));
        zones.add(new Zone(CROSS, faceX, faceY + faceOffset, faceRadius));
        zones.add(new Zone(SQUARE, faceX - faceOffset, faceY, faceRadius));

        // Shoulders sit above their thumb cluster, mirrored with the cluster.
        zones.add(new Zone(R1, faceX - faceOffset * 0.95f, faceY - faceOffset * 2.3f, smallRadius));
        zones.add(new Zone(R2, faceX + faceOffset * 0.95f, faceY - faceOffset * 2.3f, smallRadius));
        zones.add(new Zone(L1, dpadX - dpadOffset * 0.95f, dpadY - dpadOffset * 3.2f, smallRadius));
        zones.add(new Zone(L2, dpadX + dpadOffset * 0.95f, dpadY - dpadOffset * 3.2f, smallRadius));

        // Menu row in the middle of the screen, out of reach of both thumbs.
        final float menuRadius = faceRadius * 0.8f;
        zones.add(new Zone(SELECT, width * 0.44f, height * 0.10f, menuRadius));
        zones.add(new Zone(START, width * 0.56f, height * 0.10f, menuRadius));

        // Stick clicks at the bottom centre.
        final float clickRadius = faceRadius * 0.7f;
        zones.add(new Zone(L3, width * 0.47f, height * 0.90f, clickRadius));
        zones.add(new Zone(R3, width * 0.53f, height * 0.90f, clickRadius));

        invalidate();
    }

    // ---------------------------------------------------------------------
    // Touch handling
    // ---------------------------------------------------------------------
    @Override
    public boolean onTouchEvent(MotionEvent event) {
        switch (event.getActionMasked()) {
            case MotionEvent.ACTION_DOWN: {
                final int control = hitTest(event.getX(), event.getY());
                if (control == NONE) {
                    return false; // never consume touches outside our controls
                }
                lastX = event.getX();
                lastY = event.getY();
                attach(event.getPointerId(0), control, event.getX(), event.getY());
                return true;
            }
            case MotionEvent.ACTION_POINTER_DOWN: {
                final int index = event.getActionIndex();
                final int control = hitTest(event.getX(index), event.getY(index));
                if (control != NONE) {
                    attach(event.getPointerId(index), control, event.getX(index), event.getY(index));
                }
                return true; // the gesture is already ours
            }
            case MotionEvent.ACTION_MOVE: {
                for (int i = 0; i < event.getPointerCount(); i++) {
                    update(event.getPointerId(i), event.getX(i), event.getY(i));
                }
                return true;
            }
            case MotionEvent.ACTION_POINTER_UP: {
                detach(event.getPointerId(event.getActionIndex()));
                return true;
            }
            case MotionEvent.ACTION_UP: {
                detachAll();
                return true;
            }
            case MotionEvent.ACTION_CANCEL: {
                resetState();
                return true;
            }
            default:
                return true;
        }
    }

    private int hitTest(float x, float y) {
        for (int i = 0; i < zones.size(); i++) {
            Zone zone = zones.get(i);
            final float slack = zone.id == STICK ? 1.0f : 1.2f;
            if (zone.contains(x, y, slack)) {
                return zone.id;
            }
        }
        return NONE;
    }

    private void attach(int pointerId, int control, float x, float y) {
        pointerControl.put(pointerId, control);
        if (control == STICK) {
            stickPointer = pointerId;
            updateStick(x, y);
        } else {
            press(control, true);
        }
        invalidate();
    }

    private void update(int pointerId, float x, float y) {
        final int control = pointerControl.get(pointerId, NONE);
        if (control == NONE) {
            return;
        }
        if (control == STICK) {
            updateStick(x, y);
            invalidate();
            return;
        }
        // Slide-off cancels the press, keep the pointer tracked so re-entry works.
        if (!isInside(control, x, y, 1.35f)) {
            press(control, false);
            invalidate();
        } else if (!pressed[control]) {
            press(control, true);
            invalidate();
        }
    }

    private void detach(int pointerId) {
        final int control = pointerControl.get(pointerId, NONE);
        pointerControl.delete(pointerId);
        if (control == NONE) {
            return;
        }
        if (control == STICK) {
            if (stickPointer == pointerId) {
                stickPointer = NONE;
                axisX = 0;
                axisY = 0;
                publish();
            }
        } else {
            press(control, false);
        }
        invalidate();
    }

    private void detachAll() {
        pointerControl.clear();
        stickPointer = NONE;
        axisX = 0;
        axisY = 0;
        for (int i = 0; i < CONTROL_COUNT; i++) {
            pressed[i] = false;
        }
        publish();
        invalidate();
    }

    /** Clears every button/axis (used on pause, surface loss and 3-finger HUD taps). */
    public void resetState() {
        detachAll();
    }

    private void press(int control, boolean down) {
        if (control < 0 || control >= CONTROL_COUNT || pressed[control] == down) {
            return;
        }
        pressed[control] = down;
        if (down && haptics) {
            performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY);
        }
        publish();
    }

    private void updateStick(float x, float y) {
        final float dx = x - stickX;
        final float dy = y - stickY;
        final float length = (float) Math.hypot(dx, dy);
        final float limit = Math.max(stickRadius, 1.0f);
        final float deadzone = 0.12f;
        float nx = 0.0f;
        float ny = 0.0f;
        if (length > deadzone * limit) {
            final float clamped = Math.min(length, limit);
            final float scaleFactor = clamped / length;
            nx = (dx * scaleFactor) / limit;
            ny = (dy * scaleFactor) / limit;
            // Drop the deadzone and rescale the remaining travel to the full range.
            nx = rescaleAfterDeadzone(nx, deadzone);
            ny = rescaleAfterDeadzone(ny, deadzone);
        }
        final int newX = clampAxis(Math.round(nx * PadMapper.AXIS_MAX));
        final int newY = clampAxis(Math.round(ny * PadMapper.AXIS_MAX));
        if (newX != axisX || newY != axisY) {
            axisX = newX;
            axisY = newY;
            publish();
        }
    }

    private static float rescaleAfterDeadzone(float value, float deadzone) {
        final float magnitude = Math.abs(value);
        if (magnitude <= deadzone) {
            return 0.0f;
        }
        final float scaled = Math.min(1.0f, (magnitude - deadzone) / (1.0f - deadzone));
        return value < 0.0f ? -scaled : scaled;
    }

    private static int clampAxis(int value) {
        return Math.max(PadMapper.AXIS_MIN, Math.min(PadMapper.AXIS_MAX, value));
    }

    private boolean isInside(int control, float x, float y, float slack) {
        for (int i = 0; i < zones.size(); i++) {
            Zone zone = zones.get(i);
            if (zone.id == control) {
                return zone.contains(x, y, slack);
            }
        }
        return false;
    }

    private void publish() {
        if (sink == null) {
            return;
        }
        int buttons = 0;
        for (int i = 0; i < CONTROL_COUNT; i++) {
            if (!pressed[i]) {
                continue;
            }
            buttons |= bitForControl(i);
        }
        sink.onPadState(buttons, axisX, axisY, 0, 0);
    }

    private static int bitForControl(int control) {
        switch (control) {
            case DPAD_UP:
                return PadMapper.PAD_UP;
            case DPAD_DOWN:
                return PadMapper.PAD_DOWN;
            case DPAD_LEFT:
                return PadMapper.PAD_LEFT;
            case DPAD_RIGHT:
                return PadMapper.PAD_RIGHT;
            case TRIANGLE:
                return PadMapper.PAD_TRIANGLE;
            case CIRCLE:
                return PadMapper.PAD_CIRCLE;
            case CROSS:
                return PadMapper.PAD_CROSS;
            case SQUARE:
                return PadMapper.PAD_SQUARE;
            case L1:
                return PadMapper.PAD_L1;
            case L2:
                return PadMapper.PAD_L2;
            case R1:
                return PadMapper.PAD_R1;
            case R2:
                return PadMapper.PAD_R2;
            case START:
                return PadMapper.PAD_START;
            case SELECT:
                return PadMapper.PAD_SELECT;
            case L3:
                return PadMapper.PAD_L3;
            case R3:
                return PadMapper.PAD_R3;
            default:
                return 0;
        }
    }

    // ---------------------------------------------------------------------
    // Drawing
    // ---------------------------------------------------------------------
    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        final int baseAlpha = (int) (opacity * 200.0f);
        fillPaint.setColor(Color.argb(baseAlpha / 2, 255, 255, 255));
        strokePaint.setColor(Color.argb(baseAlpha, 255, 255, 255));
        textPaint.setColor(Color.argb(Math.min(255, baseAlpha + 40), 255, 255, 255));
        accentPaint.setAlpha(Math.min(255, baseAlpha + 55));

        for (int i = 0; i < zones.size(); i++) {
            Zone zone = zones.get(i);
            if (zone.id == STICK) {
                drawStick(canvas, zone);
                continue;
            }
            final boolean active = pressed[zone.id];
            canvas.drawCircle(zone.cx, zone.cy, zone.radius, active ? accentPaint : fillPaint);
            canvas.drawCircle(zone.cx, zone.cy, zone.radius, strokePaint);
            final String label = labelFor(zone.id);
            if (label != null) {
                textPaint.setTextSize(zone.radius * (label.length() > 1 ? 0.62f : 0.95f));
                final float baseline = zone.cy - (textPaint.ascent() + textPaint.descent()) * 0.5f;
                canvas.drawText(label, zone.cx, baseline, textPaint);
            }
        }
    }

    private void drawStick(Canvas canvas, Zone zone) {
        canvas.drawCircle(zone.cx, zone.cy, zone.radius, fillPaint);
        canvas.drawCircle(zone.cx, zone.cy, zone.radius, strokePaint);

        final float knobRadius = zone.radius * 0.42f;
        final float travel = zone.radius - knobRadius;
        final float knobX = zone.cx + (axisX / (float) PadMapper.AXIS_MAX) * travel;
        final float knobY = zone.cy + (axisY / (float) PadMapper.AXIS_MAX) * travel;
        canvas.drawCircle(knobX, knobY, knobRadius, strokePaint);
        accentPaint.setAlpha(Math.min(255, (int) (opacity * 160.0f)));
        canvas.drawCircle(knobX, knobY, knobRadius * 0.75f, accentPaint);
    }

    private static String labelFor(int control) {
        switch (control) {
            case TRIANGLE:
                return "\u25B3";
            case CIRCLE:
                return "\u25CB";
            case CROSS:
                return "\u2715";
            case SQUARE:
                return "\u25A1";
            case DPAD_UP:
                return "\u25B2";
            case DPAD_DOWN:
                return "\u25BC";
            case DPAD_LEFT:
                return "\u25C0";
            case DPAD_RIGHT:
                return "\u25B6";
            case L1:
                return "L1";
            case L2:
                return "L2";
            case R1:
                return "R1";
            case R2:
                return "R2";
            case START:
                return "START";
            case SELECT:
                return "SELECT";
            case L3:
                return "L3";
            case R3:
                return "R3";
            default:
                return null;
        }
    }
}
