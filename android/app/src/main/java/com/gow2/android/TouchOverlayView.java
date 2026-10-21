package com.gow2.android;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.view.MotionEvent;
import android.view.View;

import java.util.HashMap;
import java.util.Map;

/**
 * A transparent, always-on gamepad over the SDL surface. It is intentionally
 * simple and screen-relative, with no bitmap assets, so it scales from small
 * phones to tablets.
 */
public final class TouchOverlayView extends View {
    private static final int BTN_SELECT = 1 << 0;
    private static final int BTN_START = 1 << 3;
    private static final int BTN_UP = 1 << 4;
    private static final int BTN_RIGHT = 1 << 5;
    private static final int BTN_DOWN = 1 << 6;
    private static final int BTN_LEFT = 1 << 7;
    private static final int BTN_L2 = 1 << 8;
    private static final int BTN_R2 = 1 << 9;
    private static final int BTN_L1 = 1 << 10;
    private static final int BTN_R1 = 1 << 11;
    private static final int BTN_TRIANGLE = 1 << 12;
    private static final int BTN_CIRCLE = 1 << 13;
    private static final int BTN_CROSS = 1 << 14;
    private static final int BTN_SQUARE = 1 << 15;

    private static native void nativeSetPad(int buttons, int lx, int ly, int rx, int ry);

    private static final class Pointer {
        int kind; // 1 = left stick, 2 = right stick, 3 = button
        int button;
        float x;
        float y;
    }

    private final Map<Integer, Pointer> pointers = new HashMap<>();
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint stroke = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF tmpRect = new RectF();

    private float leftCx, leftCy, leftR;
    private float rightCx, rightCy, rightR;
    private float faceCx, faceCy, faceR, faceGap;
    private final RectF l1 = new RectF(), l2 = new RectF(), r1 = new RectF(), r2 = new RectF();
    private final RectF select = new RectF(), start = new RectF();
    private boolean leftActive, rightActive;
    private int pressedButtons;
    private float leftX = 128, leftY = 128, rightX = 128, rightY = 128;

    public TouchOverlayView(Context context) {
        super(context);
        setFocusable(false);
        setClickable(false);
        fill.setStyle(Paint.Style.FILL);
        stroke.setStyle(Paint.Style.STROKE);
        stroke.setStrokeWidth(dp(2.0f));
        text.setColor(Color.argb(235, 245, 236, 220));
        text.setTextAlign(Paint.Align.CENTER);
        text.setTypeface(Typeface.create("sans-serif", Typeface.BOLD));
        setWillNotDraw(false);
    }

    private float dp(float v) {
        return v * getResources().getDisplayMetrics().density;
    }

    @Override
    protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        super.onSizeChanged(w, h, oldw, oldh);
        final float unit = Math.min(w, h);
        leftR = unit * 0.15f;
        leftCx = w * 0.17f;
        leftCy = h * 0.70f;
        rightR = unit * 0.12f;
        rightCx = w * 0.68f;
        rightCy = h * 0.70f;
        faceCx = w * 0.86f;
        faceCy = h * 0.63f;
        faceR = unit * 0.052f;
        faceGap = unit * 0.070f;

        final float shoulderW = w * 0.15f;
        final float shoulderH = h * 0.065f;
        l1.set(w * 0.035f, h * 0.28f, w * 0.035f + shoulderW, h * 0.28f + shoulderH);
        l2.set(w * 0.035f, h * 0.28f + shoulderH + dp(5), w * 0.035f + shoulderW, h * 0.28f + shoulderH * 2 + dp(5));
        r1.set(w * 0.965f - shoulderW, h * 0.28f, w * 0.965f, h * 0.28f + shoulderH);
        r2.set(w * 0.965f - shoulderW, h * 0.28f + shoulderH + dp(5), w * 0.965f, h * 0.28f + shoulderH * 2 + dp(5));

        final float smallW = w * 0.085f;
        final float smallH = h * 0.070f;
        select.set(w * 0.36f, h * 0.91f, w * 0.36f + smallW, h * 0.91f + smallH);
        start.set(w * 0.36f + smallW + dp(10), h * 0.91f, w * 0.36f + smallW * 2 + dp(10), h * 0.91f + smallH);
    }

    private int hitButton(float x, float y) {
        if (l1.contains(x, y)) return BTN_L1;
        if (l2.contains(x, y)) return BTN_L2;
        if (r1.contains(x, y)) return BTN_R1;
        if (r2.contains(x, y)) return BTN_R2;
        if (select.contains(x, y)) return BTN_SELECT;
        if (start.contains(x, y)) return BTN_START;
        final float dx = x - faceCx, dy = y - faceCy;
        if (dx * dx + dy * dy <= (faceGap + faceR) * (faceGap + faceR)) {
            if (Math.abs(dx) > Math.abs(dy)) return dx > 0 ? BTN_CIRCLE : BTN_SQUARE;
            return dy < 0 ? BTN_TRIANGLE : BTN_CROSS;
        }
        return 0;
    }

    private void addPointer(int id, float x, float y) {
        Pointer p = new Pointer();
        p.x = x;
        p.y = y;
        final float ldx = x - leftCx, ldy = y - leftCy;
        final float rdx = x - rightCx, rdy = y - rightCy;
        if (ldx * ldx + ldy * ldy <= leftR * leftR * 1.45f) {
            p.kind = 1;
        } else if (rdx * rdx + rdy * rdy <= rightR * rightR * 1.65f) {
            p.kind = 2;
        } else {
            p.button = hitButton(x, y);
            p.kind = p.button != 0 ? 3 : 0;
        }
        pointers.put(id, p);
    }

    private void movePointer(int id, float x, float y) {
        Pointer p = pointers.get(id);
        if (p == null) return;
        p.x = x;
        p.y = y;
    }

    private void removePointer(int id) {
        pointers.remove(id);
    }

    private void publish() {
        int buttons = 0;
        float lx = 128, ly = 128, rx = 128, ry = 128;
        boolean ls = false, rs = false;
        for (Pointer p : pointers.values()) {
            if (p.kind == 3) {
                buttons |= p.button;
            } else if (p.kind == 1) {
                ls = true;
                lx = axis(p.x - leftCx, leftR);
                ly = axis(p.y - leftCy, leftR);
                if (lx < 70) buttons |= BTN_LEFT;
                if (lx > 186) buttons |= BTN_RIGHT;
                if (ly < 70) buttons |= BTN_UP;
                if (ly > 186) buttons |= BTN_DOWN;
            } else if (p.kind == 2) {
                rs = true;
                rx = axis(p.x - rightCx, rightR);
                ry = axis(p.y - rightCy, rightR);
            }
        }
        pressedButtons = buttons;
        leftActive = ls;
        rightActive = rs;
        leftX = ls ? lx : 128;
        leftY = ls ? ly : 128;
        rightX = rs ? rx : 128;
        rightY = rs ? ry : 128;
        nativeSetPad(buttons, (int) leftX, (int) leftY, (int) rightX, (int) rightY);
        invalidate();
    }

    private float axis(float delta, float radius) {
        final float v = 128.0f + (delta / radius) * 127.0f;
        return Math.max(0.0f, Math.min(255.0f, v));
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        final int action = event.getActionMasked();
        if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) {
            final int index = event.getActionIndex();
            addPointer(event.getPointerId(index), event.getX(index), event.getY(index));
            publish();
            return true;
        }
        if (action == MotionEvent.ACTION_MOVE) {
            for (int i = 0; i < event.getPointerCount(); ++i)
                movePointer(event.getPointerId(i), event.getX(i), event.getY(i));
            publish();
            return true;
        }
        if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP || action == MotionEvent.ACTION_CANCEL) {
            final int index = event.getActionIndex();
            removePointer(event.getPointerId(index));
            publish();
            return true;
        }
        return true;
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        drawStick(canvas, leftCx, leftCy, leftR, leftActive, leftX, leftY, false);
        drawStick(canvas, rightCx, rightCy, rightR, rightActive, rightX, rightY, true);

        drawFace(canvas, faceCx, faceCy - faceGap, faceR, "△", (pressedButtons & BTN_TRIANGLE) != 0);
        drawFace(canvas, faceCx + faceGap, faceCy, faceR, "○", (pressedButtons & BTN_CIRCLE) != 0);
        drawFace(canvas, faceCx, faceCy + faceGap, faceR, "✕", (pressedButtons & BTN_CROSS) != 0);
        drawFace(canvas, faceCx - faceGap, faceCy, faceR, "□", (pressedButtons & BTN_SQUARE) != 0);

        drawButton(canvas, l1, "L1", (pressedButtons & BTN_L1) != 0);
        drawButton(canvas, l2, "L2", (pressedButtons & BTN_L2) != 0);
        drawButton(canvas, r1, "R1", (pressedButtons & BTN_R1) != 0);
        drawButton(canvas, r2, "R2", (pressedButtons & BTN_R2) != 0);
        drawButton(canvas, select, "SELECT", (pressedButtons & BTN_SELECT) != 0);
        drawButton(canvas, start, "START", (pressedButtons & BTN_START) != 0);
    }

    private void drawStick(Canvas canvas, float cx, float cy, float r, boolean active, float ax, float ay, boolean camera) {
        fill.setColor(Color.argb(active ? 55 : 30, 245, 236, 220));
        canvas.drawCircle(cx, cy, r, fill);
        stroke.setColor(Color.argb(active ? 190 : 95, 225, 190, 130));
        canvas.drawCircle(cx, cy, r, stroke);
        stroke.setColor(Color.argb(110, 245, 236, 220));
        canvas.drawCircle(cx, cy, r * 0.58f, stroke);
        final float kx = cx + (ax - 128.0f) / 127.0f * r * 0.55f;
        final float ky = cy + (ay - 128.0f) / 127.0f * r * 0.55f;
        fill.setColor(Color.argb(active ? 155 : 95, 235, 218, 190));
        canvas.drawCircle(kx, ky, r * 0.22f, fill);
        if (camera) {
            text.setTextSize(r * 0.16f);
            text.setColor(Color.argb(130, 240, 230, 215));
            canvas.drawText("CAM", cx, cy + r * 0.07f, text);
        }
    }

    private void drawFace(Canvas canvas, float cx, float cy, float r, String label, boolean pressed) {
        fill.setColor(Color.argb(pressed ? 150 : 55, 245, 236, 220));
        canvas.drawCircle(cx, cy, r, fill);
        stroke.setColor(Color.argb(pressed ? 240 : 175, 225, 190, 130));
        canvas.drawCircle(cx, cy, r, stroke);
        text.setColor(Color.argb(245, 250, 244, 232));
        text.setTextSize(r * 0.75f);
        final float baseline = cy - (text.descent() + text.ascent()) / 2.0f;
        canvas.drawText(label, cx, baseline, text);
    }

    private void drawButton(Canvas canvas, RectF r, String label, boolean pressed) {
        fill.setColor(Color.argb(pressed ? 150 : 45, 245, 236, 220));
        canvas.drawRoundRect(r, dp(9), dp(9), fill);
        stroke.setColor(Color.argb(pressed ? 230 : 150, 225, 190, 130));
        canvas.drawRoundRect(r, dp(9), dp(9), stroke);
        text.setColor(Color.argb(235, 245, 236, 220));
        text.setTextSize(r.height() * 0.40f);
        final float baseline = r.centerY() - (text.descent() + text.ascent()) / 2.0f;
        canvas.drawText(label, r.centerX(), baseline, text);
    }
}
