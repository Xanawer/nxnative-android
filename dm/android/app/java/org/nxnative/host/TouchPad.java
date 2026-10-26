// The touch gamepad (docs/android/PLAN.md 1.6): dm/ios/Sources/DmGamepad.m's 2D layout, its constants, hit tests and look, as a view in a window of its own over the activity's.
//   ZL above L at the top left and ZR above R at the top right (teardrops, tip toward the corner), − and + side by side at the top centre, the left stick in the bottom-left corner (a fixed
//   ring, tilt measured from its centre, a small dead zone), the A (right) B (bottom) X (top) Y (left) diamond in the bottom-right corner, where sliding a finger between keys switches keys.
//   A touch-down takes, in order: a shoulder or − / + (held until it lifts), else whichever of the diamond and the stick has the smaller distance / grab radius, else the game's touch screen.
//   The controls hide while a game controller is connected and come back when it is removed, and hide in a window too small for them; meanwhile every touch goes to the game.
// Why a window: NativeActivity takes its window's surface and input queue, so a view in the activity's own window is never drawn (it would still get the touches the native queue leaves
// unhandled). A TYPE_APPLICATION_PANEL window attached to the activity's draws over the game's picture and takes the touches on it; FLAG_NOT_FOCUSABLE keeps the focus, and with it the keys
// and controller events, on the activity's window and its native input queue, and leaves the system keyboard's box (a dialog) and the keyboard itself above the panel (probed, PLAN.md 1.6).
// The panel covers the whole activity window, so every touch comes here: after each touch event the whole touch state (the buttons and stick the controls hold, the fingers on the game)
// goes to front_android.c in one call (HostActivity.nativeTouchSet), which merges it with the controller's. Everything here runs on the UI thread, as the native input queue's callbacks do
package org.nxnative.host;

import android.app.Activity;
import android.graphics.Canvas;
import android.graphics.Insets;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.hardware.input.InputManager;
import android.view.InputDevice;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowManager;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.Collections;

final class TouchPad extends View implements InputManager.InputDeviceListener {
    // The keys, as their dm_input.h bit numbers (DM_BTN_A = 1 << 0, ...)
    private static final int A = 0, B = 1, X = 2, Y = 3, L = 6, R = 7, ZL = 8, ZR = 9, PLUS = 10, MINUS = 11, NKEY = 12;
    private static final int[] LOCKED = { ZL, L, ZR, R, MINUS, PLUS };   // tested on touch-down only, then held until the touch lifts
    private static final String[] NAME = new String[NKEY];
    static { NAME[A] = "A"; NAME[B] = "B"; NAME[X] = "X"; NAME[Y] = "Y"; NAME[L] = "L"; NAME[R] = "R"; NAME[ZL] = "ZL"; NAME[ZR] = "ZR"; NAME[PLUS] = "+"; NAME[MINUS] = "−"; }
    // DmGamepad.m's layout constants, in u (x measured from the outer edge, the right half mirrors)
    private static final float K_ZL_X = 44, K_ZL_Y = 40, K_ZL_R = 26, K_L_X = 44, K_L_Y = 108, K_L_R = 24, K_DROP_TIP = 1.45f, K_CORNER_IN = 0.5f,
        K_FACE_X = 81, K_PAD_Y = 82, K_FACE_GAP = 50, K_FACE_R = 27, K_PILL_DX = 52, K_PILL_Y = 22, K_PILL_W = 50, K_PILL_H = 28,
        K_STICK_X = 81, K_STICK_R = 77, K_KNOB_R = 30, K_STICK_GRAB = 1.4f, K_FULL = 0.8f, K_DEAD = 0.1f,
        // The least room, in u, that keeps every control's hit region clear of the others' and of their shapes, with about 10% to spare (layout(); found by an offline sweep of this layout,
        // PLAN.md 1.6): between the bottom corners 363.4 u (the stick's grab circle and its knob at the ring's edge against the diamond's grab, 81 + 77 x 1.4 + 50 + 27 x 1.6 + 81, plus the
        // stroke); from the window's centre to each side of the safe area 164.1 u (a shoulder's hit circle against the − / + pill inflated by 8 u, 44 + 26 x 1.35 + 52 + 25 + 8); from the safe
        // area's top to its bottom 330.5 u (L's hit circle above the stick's grab circle and knob, 108 + 24 x 1.35 + 82 + 77 x 1.4, plus the stroke)
        K_CORNERS_W = 400, K_HALF_W = 180, K_SAFE_H = 365,
        U_MIN = 0.6f;   // dp: below it the controls hide (face keys 32 dp across, their hit circles 40 dp; the pills' hit height 26 dp)
    private static final int STICK_MAX = 32767;   // dm_input.h DM_STICK_MAX

    // What a finger holds: a diamond touch (its keys follow the finger), the stick, one locked key, a finger on the game's touch screen (slot), or nothing until it lifts
    private static final int F_FACE = 1, F_STICK = 2, F_KEY = 3, F_GAME = 4, F_IGNORED = 5;
    private static final class Finger { int kind, key, face, slot; float x, y; }
    private final Finger[] fingers = new Finger[32];   // by pointer id (MotionEvent ids are 0..31)
    private final boolean[] slotUsed = new boolean[16];   // the game's finger slots (dm_touch ids): the lowest free one on touch-down, kept until the finger lifts
    private int stickId = -1;                            // the pointer holding the stick
    private int held, lx, ly;                            // the last state handed over (buttons, stick)
    private float knobDx, knobDy;                        // the knob's offset from the ring centre
    private boolean knobHeld;                            // the knob was drawn held (its fill)
    private int epoch;                                   // front_android.c's release count, as last seen
    private boolean padOff;                              // a game controller is connected: no controls, every touch goes to the game
    private boolean small;                               // the window leaves u under U_MIN: no controls (u is 0), every touch goes to the game
    private final float[] game = new float[48];

    // The layout, in pixels (layout())
    private float u, faceX, faceY, gap, keyR, stickX, stickY, stickR, knobR, lineW;
    private final float[] cx = new float[NKEY], cy = new float[NKEY], cr = new float[NKEY];   // the diamond keys' and teardrops' centres and radii
    private final RectF[] pill = new RectF[NKEY];
    private final Path[] drop = new Path[NKEY];
    private Insets cut = Insets.NONE, bars = Insets.NONE;   // the display cutout's and the system bars' insets, kept apart (layout())
    private WindowManager wm;
    private WindowManager.LayoutParams lp;
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), line = new Paint(Paint.ANTI_ALIAS_FLAG), text = new Paint(Paint.ANTI_ALIAS_FLAG);
    // DmGamepad.m's colours: each carries its own low opacity
    private static final int C_IDLE = 0x24ffffff, C_ON = 0x73ffffff, C_LINE = 0x61ffffff, C_RING = 0x0fffffff, C_TEXT = 0x99ffffff, C_TEXT_ON = 0xb3000000;

    private TouchPad(Activity a) {
        super(a);
        line.setStyle(Paint.Style.STROKE); line.setColor(C_LINE);
        text.setTextAlign(Paint.Align.CENTER); text.setTypeface(Typeface.DEFAULT_BOLD);
    }

    // The panel over the activity's window (its onAttachedToWindow): null when the host did not start (front_android.c binds the natives only then: nothing to control) or the window is gone
    static TouchPad attach(Activity a) {
        try { HostActivity.nativeTouchEpoch(); } catch (UnsatisfiedLinkError e) { return null; }
        TouchPad v = new TouchPad(a);
        WindowManager.LayoutParams lp = new WindowManager.LayoutParams(WindowManager.LayoutParams.MATCH_PARENT, WindowManager.LayoutParams.MATCH_PARENT,
            WindowManager.LayoutParams.TYPE_APPLICATION_PANEL,
            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE | WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL | WindowManager.LayoutParams.FLAG_SPLIT_TOUCH
                | WindowManager.LayoutParams.FLAG_HARDWARE_ACCELERATED | (a.hasWindowFocus() ? 0 : WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE),
            PixelFormat.TRANSLUCENT);
        lp.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS;   // the activity's whole window, the cutout's side included; the controls keep out of it (insets)
        lp.setFitInsetsTypes(0);
        lp.token = a.getWindow().getDecorView().getWindowToken();
        lp.setTitle("nxnative touch controls");
        try { a.getWindowManager().addView(v, lp); } catch (WindowManager.BadTokenException e) { return null; }
        v.wm = a.getWindowManager(); v.lp = lp;
        a.getSystemService(InputManager.class).registerInputDeviceListener(v, null);   // on this (the UI) thread's looper
        v.devicesChanged(true);
        return v;
    }

    // The activity's window focus (HostActivity.onWindowFocusChanged). The panel takes no focus, and the system gives focus only to a window that can take keys: in multi-window, a tap on
    // a panel covering the activity would never bring focus (the controller's keys) back from another app. So while the activity lacks focus the panel lets touches through to the activity's
    // window, whose first tap focuses it (the native queue leaves that touch unhandled), and takes them again once focus is back. Without focus nothing is held (release, front_android.c on_focus)
    void focus(boolean has) {
        if (!has) release();
        boolean off = (lp.flags & WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE) != 0;
        if (off != has) return;
        lp.flags ^= WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE;
        try { wm.updateViewLayout(this, lp); } catch (IllegalArgumentException e) { return; }   // the panel already gone with the window
        log(has ? "touch controls take touches again (the activity has focus)" : "touch controls let touches through to the activity's window (it lost focus)");
    }

    private static void log(String s) { HostActivity.nativeTouchLog(s.getBytes(StandardCharsets.UTF_8)); }   // the host's log under DM_INPUT_LOG

    // ---- Game controllers (InputManager): a non-virtual device with the gamepad or joystick source (the whole source value: every keyboard shares the button class with gamepads) ----
    private static String controller() {
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice d = InputDevice.getDevice(id);
            if (d == null || d.isVirtual()) continue;
            int s = d.getSources();
            if ((s & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD || (s & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK) return d.getName();
        }
        return null;
    }
    private void devicesChanged(boolean first) {
        String name = controller();
        if (!first && (name != null) == padOff) return;
        padOff = name != null;
        release();
        exclude();
        log(padOff ? "touch controls hidden: game controller '" + name + "' connected (every touch goes to the game)"
                   : small ? "no game controller connected, but the touch controls stay hidden: the window has no room for them" : "touch controls shown: no game controller connected");
    }
    @Override public void onInputDeviceAdded(int id) { devicesChanged(false); }
    // A device gone mid-tilt sends no last motion: front_android.c lets go of the controller's sticks, hat and triggers if they were that device's, and of its keys too when it was the last
    // controller (padOff still says whether one was connected; the removed device is already out of InputDevice's list)
    @Override public void onInputDeviceRemoved(int id) { HostActivity.nativeControllerRemoved(id, padOff && controller() == null); devicesChanged(false); }
    @Override public void onInputDeviceChanged(int id) { devicesChanged(false); }

    // ---- Layout (DmGamepad.m: u = 1 at 430 points tall, at most 1.3; the controls inside the safe area, the bottom corners reaching halfway into the side insets) ----
    // u is also at most what the safe area allows the hit regions without meeting (K_CORNERS_W, K_HALF_W, K_SAFE_H). iOS scales by height alone and never needs it: full screen in landscape,
    // an iPhone is at least 764.83 u wide and an iPad at the 1.3 cap more. A narrow multi-window window (320 x 600 dp: u 1.3 dp, 246 u wide) would otherwise put + inside ZR's hit circle (ZR
    // is tested first) and Y within the stick's grab; a short one with tall bars (640 x 220 dp, 32 and 48 dp insets) X inside R's. Under U_MIN the controls hide (PLAN.md 1.6)
    @Override public WindowInsets onApplyWindowInsets(WindowInsets in) {
        Insets c = in.getInsets(WindowInsets.Type.displayCutout()), b = in.getInsets(WindowInsets.Type.systemBars());
        if (!c.equals(cut) || !b.equals(bars)) { cut = c; bars = b; layout(); }
        return in;
    }
    @Override protected void onSizeChanged(int w, int h, int ow, int oh) { layout(); }   // the window's size (the Fold's two displays, multi-window), not the screen's
    private void layout() {
        int w = getWidth(), h = getHeight();
        if (w <= 0 || h <= 0) return;
        float d = getResources().getDisplayMetrics().density;
        // The safe area: clear of the cutout and the system bars. The bottom corners reach halfway into a side's cutout inset (a punch hole is mid-edge, as iOS's Dynamic Island), never under a
        // side's bar (3-button navigation in landscape: its Back key ends the game)
        float sl = Math.max(cut.left, bars.left), st = Math.max(cut.top, bars.top), sr = w - Math.max(cut.right, bars.right), sb = h - Math.max(cut.bottom, bars.bottom);
        float cl = Math.max(cut.left * K_CORNER_IN, bars.left), crt = w - Math.max(cut.right * K_CORNER_IN, bars.right);
        float mid = w / 2f;   // − / + about the window's centre: a cutout's inset is on one side only (iOS's safe area is symmetric), and the pair would shift with the rotation
        // These depend on the insets alone, so the room they leave bounds u: the bottom corners' span, the narrower half of the top row, the safe area's height
        u = Math.max(0, Math.min(Math.min(h / 430f, 1.3f * d), Math.min(Math.min((crt - cl) / K_CORNERS_W, Math.min(sr - mid, mid - sl) / K_HALF_W), (sb - st) / K_SAFE_H)));
        boolean sm = u < U_MIN * d;
        if (sm != small) {
            small = sm;
            log(sm ? String.format(java.util.Locale.ROOT, "touch controls hidden: the window leaves them u %.2f dp, under %.1f dp (every touch goes to the game)", u / d, U_MIN)
                   : padOff ? "the window has room for the touch controls again, but they stay hidden: a game controller is connected" : "touch controls shown: the window has room for them again");
        }
        if (sm) u = 0;   // no controls: begin() sends every touch to the game, onDraw() and exclude() leave the window alone
        lineW = 1.5f * d; line.setStrokeWidth(lineW);
        gap = K_FACE_GAP * u; keyR = K_FACE_R * u;
        faceX = crt - K_FACE_X * u; faceY = sb - K_PAD_Y * u;
        place(A, faceX + gap, faceY, keyR); place(B, faceX, faceY + gap, keyR); place(X, faceX, faceY - gap, keyR); place(Y, faceX - gap, faceY, keyR);
        stickX = cl + K_STICK_X * u; stickY = sb - K_PAD_Y * u; stickR = K_STICK_R * u; knobR = K_KNOB_R * u;
        place(ZL, sl + K_ZL_X * u, st + K_ZL_Y * u, K_ZL_R * u); place(L, sl + K_L_X * u, st + K_L_Y * u, K_L_R * u);
        place(ZR, sr - K_ZL_X * u, st + K_ZL_Y * u, K_ZL_R * u); place(R, sr - K_L_X * u, st + K_L_Y * u, K_L_R * u);
        for (int k : new int[] { ZL, L }) drop[k] = drop(cx[k], cy[k], cr[k], -3 * Math.PI / 4, 4 * u);   // tip toward the near top corner
        for (int k : new int[] { ZR, R }) drop[k] = drop(cx[k], cy[k], cr[k], -Math.PI / 4, 4 * u);
        for (int k : new int[] { MINUS, PLUS }) {   // − on the left, + on the right
            float x = mid + (k == PLUS ? K_PILL_DX : -K_PILL_DX) * u, y = st + K_PILL_Y * u, pw = K_PILL_W * u / 2, ph = K_PILL_H * u / 2;
            pill[k] = new RectF(x - pw, y - ph, x + pw, y + ph); cx[k] = x; cy[k] = y;
        }
        exclude();
        StringBuilder s = new StringBuilder();
        s.append(String.format(java.util.Locale.ROOT, "touch controls laid out in %dx%d (cutout %d,%d,%d,%d, bars %d,%d,%d,%d; u %.2f px):", w, h, cut.left, cut.top, cut.right, cut.bottom,
                               bars.left, bars.top, bars.right, bars.bottom, u));
        for (int k : new int[] { A, B, X, Y, ZL, L, ZR, R, MINUS, PLUS }) s.append(String.format(java.util.Locale.ROOT, " %s %.0f,%.0f", NAME[k], cx[k], cy[k]));
        s.append(String.format(java.util.Locale.ROOT, " stick %.0f,%.0f radius %.0f", stickX, stickY, stickR));
        log(s.toString());
        release();   // the controls moved: nothing stays held at a place no longer under the finger
    }
    // The system's back gesture starts at the side edges, where the stick and the diamond reach: excluded there while they are shown, so a slide on them stays theirs (the system keeps at most
    // 200 dp per edge); hidden controls (a controller connected) leave the gesture alone
    private void exclude() {
        if (padOff || u <= 0) { setSystemGestureExclusionRects(Collections.<Rect>emptyList()); return; }
        float sg = stickR * K_STICK_GRAB, fg = gap + keyR * 1.6f;
        setSystemGestureExclusionRects(Arrays.asList(new Rect((int) (stickX - sg), (int) (stickY - sg), (int) (stickX + sg), (int) (stickY + sg)),
                                                     new Rect((int) (faceX - fg), (int) (faceY - fg), (int) (faceX + fg), (int) (faceY + fg))));
    }
    private void place(int k, float x, float y, float r) { cx[k] = x; cy[k] = y; cr[k] = r; }
    // A teardrop: the circle and two tangents meeting at the tip, the tip rounded (DmGamepad.m dropPath)
    private static Path drop(float x, float y, float r, double tip, float round) {
        double dd = r * K_DROP_TIP, th = Math.acos(r / dd);
        float tx = (float) (x + dd * Math.cos(tip)), ty = (float) (y + dd * Math.sin(tip));
        float p1x = (float) (x + r * Math.cos(tip + th)), p1y = (float) (y + r * Math.sin(tip + th)), p2x = (float) (x + r * Math.cos(tip - th)), p2y = (float) (y + r * Math.sin(tip - th));
        float side = (float) Math.hypot(tx - p2x, ty - p2y), t = (float) (round / Math.tan(Math.asin(r / dd))) / side;   // where the rounding meets each side, as a fraction of it
        Path p = new Path();
        p.arcTo(new RectF(x - r, y - r, x + r, y + r), (float) Math.toDegrees(tip + th), (float) Math.toDegrees(2 * Math.PI - 2 * th), true);   // from p1 the long way round to p2
        p.lineTo(tx + (p2x - tx) * t, ty + (p2y - ty) * t);
        p.quadTo(tx, ty, tx + (p1x - tx) * t, ty + (p1y - ty) * t);
        p.close();
        return p;
    }

    // ---- Touch ----
    private static float dist(float ax, float ay, float bx, float by) { return (float) Math.hypot(ax - bx, ay - by); }
    private int lockedAt(float x, float y) {
        for (int k : LOCKED) {
            boolean hit = pill[k] != null ? x >= pill[k].left - 8 * u && x <= pill[k].right + 8 * u && y >= pill[k].top - 8 * u && y <= pill[k].bottom + 8 * u
                                          : dist(x, y, cx[k], cy[k]) <= cr[k] * 1.35f;
            if (hit) return k;
        }
        return -1;
    }
    private int faceAt(float x, float y) {   // the diamond keys under the finger (between two keys, both)
        int m = 0;
        for (int k = A; k <= Y; k++) if (dist(x, y, cx[k], cy[k]) <= keyR * 1.25f) m |= 1 << k;
        return m;
    }
    private void begin(int id, float x, float y) {
        Finger f = new Finger(); f.x = x; f.y = y;
        if (!padOff && u > 0) {
            int k = lockedAt(x, y);
            if (k >= 0) { f.kind = F_KEY; f.key = k; }
            else {   // the diamond or the stick, whichever has the smaller distance / grab radius, if it is at most 1
                float best = 1, fd = dist(x, y, faceX, faceY) / (gap + keyR * 1.6f);
                if (fd <= best) { best = fd; f.kind = F_FACE; }
                if (stickId < 0 && dist(x, y, stickX, stickY) / (stickR * K_STICK_GRAB) < best) f.kind = F_STICK;
                if (f.kind == F_FACE) f.face = faceAt(x, y);
                else if (f.kind == F_STICK) stickId = id;
            }
        }
        if (f.kind == 0) {   // the game's touch screen
            f.kind = F_IGNORED;
            for (int s = 0; s < 16; s++) if (!slotUsed[s]) { slotUsed[s] = true; f.kind = F_GAME; f.slot = s; break; }
        }
        fingers[id] = f;
    }
    private void end(int id) {
        Finger f = fingers[id];
        if (f == null) return;
        if (f.kind == F_GAME) slotUsed[f.slot] = false;
        if (id == stickId) stickId = -1;
        fingers[id] = null;
    }
    // Let go of everything held (a controller connected, the layout changed, focus lost, a release in front_android.c): the fingers down stay untracked until they lift
    void release() {
        for (Finger f : fingers) if (f != null) { if (f.kind == F_GAME) slotUsed[f.slot] = false; f.kind = F_IGNORED; f.face = 0; }
        stickId = -1;
        publish();
        invalidate();
    }
    @Override public boolean onTouchEvent(MotionEvent e) {
        int ep = HostActivity.nativeTouchEpoch();
        if (ep != epoch) { epoch = ep; release(); }
        int act = e.getActionMasked(), i = e.getActionIndex();
        switch (act) {
            case MotionEvent.ACTION_DOWN: case MotionEvent.ACTION_POINTER_DOWN: {
                if (act == MotionEvent.ACTION_DOWN) for (int id = 0; id < fingers.length; id++) end(id);   // a new gesture: nothing of an earlier one is down
                int id = e.getPointerId(i);
                if (id < fingers.length) { end(id); begin(id, e.getX(i), e.getY(i)); }
                break;
            }
            case MotionEvent.ACTION_MOVE:
                for (int p = 0; p < e.getPointerCount(); p++) {
                    int id = e.getPointerId(p); Finger f = id < fingers.length ? fingers[id] : null;
                    if (f == null) continue;
                    f.x = e.getX(p); f.y = e.getY(p);
                    if (f.kind == F_FACE) f.face = faceAt(f.x, f.y);
                }
                break;
            case MotionEvent.ACTION_UP: case MotionEvent.ACTION_POINTER_UP: {
                int id = e.getPointerId(i);
                if (id < fingers.length) end(id);
                break;
            }
            case MotionEvent.ACTION_CANCEL:
                for (int id = 0; id < fingers.length; id++) end(id);
                break;
            default: return true;
        }
        publish();
        return true;
    }
    // The whole touch state to front_android.c (which pushes only what changed), and a redraw when the controls' look changed
    private void publish() {
        int face = 0, keys = 0, n = 0;
        for (Finger f : fingers) if (f != null) { if (f.kind == F_FACE) face |= f.face; else if (f.kind == F_KEY) keys |= 1 << f.key; }
        int nlx = 0, nly = 0; float kdx = 0, kdy = 0;
        Finger s = stickId >= 0 ? fingers[stickId] : null;
        if (s != null) {   // the knob follows the finger up to the ring; outside the dead zone the tilt rises from 0 to full at the full-tilt radius (up is positive for the host)
            float dx = s.x - stickX, dy = s.y - stickY, d = (float) Math.hypot(dx, dy);
            if (d > stickR) { dx *= stickR / d; dy *= stickR / d; d = stickR; }
            kdx = dx; kdy = dy;
            float dz = stickR * K_DEAD;
            if (d > dz) { float m = Math.min((d - dz) / (stickR * K_FULL - dz), 1) / d; nlx = Math.round(dx * m * STICK_MAX); nly = Math.round(-dy * m * STICK_MAX); }
        }
        boolean redraw = (face | keys) != held || kdx != knobDx || kdy != knobDy || (s != null) != knobHeld;   // the knob's fill shows the grip, even at the ring's centre
        held = face | keys; lx = nlx; ly = nly; knobDx = kdx; knobDy = kdy; knobHeld = s != null;
        // The fingers on the game, normalized on the 16:9 picture centred in the window (dm/gfx/vkn/vk_present.c), as DmGamepad.m pushGame
        float w = getWidth(), h = getHeight(), pw = w, ph = h;
        if (w > 0 && h > 0) {
            if (w / h > 16f / 9f) pw = h * 16f / 9f; else ph = w * 9f / 16f;
            float x0 = (w - pw) / 2, y0 = (h - ph) / 2;
            for (Finger f : fingers) if (f != null && f.kind == F_GAME) {
                game[n * 3] = f.slot; game[n * 3 + 1] = Math.min(Math.max((f.x - x0) / pw, 0f), 0.999f); game[n * 3 + 2] = Math.min(Math.max((f.y - y0) / ph, 0f), 0.999f); n++;
            }
        }
        HostActivity.nativeTouchSet(held, lx, ly, game, n);
        if (redraw) invalidate();
    }

    // ---- Drawing: only when a control's look changes (a press, the knob) ----
    @Override protected void onDraw(Canvas c) {
        if (padOff || u <= 0) return;
        fill.setColor(C_RING); c.drawCircle(stickX, stickY, stickR, fill); c.drawCircle(stickX, stickY, stickR, line);
        float kx = stickX + knobDx, ky = stickY + knobDy;
        fill.setColor(knobHeld ? C_ON : C_IDLE); c.drawCircle(kx, ky, knobR, fill); c.drawCircle(kx, ky, knobR, line); c.drawCircle(kx, ky, knobR * 0.62f, line);
        for (int k = A; k <= Y; k++) { key(c, k); c.drawCircle(cx[k], cy[k], cr[k], fill); c.drawCircle(cx[k], cy[k], cr[k], line); label(c, k, 20 * u); }
        for (int k : new int[] { ZL, L, ZR, R }) { key(c, k); c.drawPath(drop[k], fill); c.drawPath(drop[k], line); label(c, k, 16 * u); }
        for (int k : new int[] { MINUS, PLUS }) { key(c, k); float r = pill[k].height() / 2; c.drawRoundRect(pill[k], r, r, fill); c.drawRoundRect(pill[k], r, r, line); label(c, k, 18 * u); }
    }
    private void key(Canvas c, int k) { boolean on = (held >> k & 1) != 0; fill.setColor(on ? C_ON : C_IDLE); text.setColor(on ? C_TEXT_ON : C_TEXT); }
    private void label(Canvas c, int k, float size) {
        text.setTextSize(size);
        Paint.FontMetrics m = text.getFontMetrics();
        c.drawText(NAME[k], cx[k], cy[k] - (m.ascent + m.descent) / 2, text);
    }
}
