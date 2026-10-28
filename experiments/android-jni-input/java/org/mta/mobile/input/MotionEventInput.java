package org.mta.mobile.input;

import android.view.InputDevice;
import android.view.MotionEvent;

/** Synchronous snapshot at the GameView.onTouchEvent seam; does not replace its return value. */
public final class MotionEventInput {
    private static final int MAX_POINTERS = 32;
    private final NativeInputBridge bridge;
    private final int[] ids = new int[MAX_POINTERS];
    private final float[] xs = new float[MAX_POINTERS];
    private final float[] ys = new float[MAX_POINTERS];

    public MotionEventInput(NativeInputBridge bridge) {
        if (bridge == null) throw new NullPointerException("bridge");
        this.bridge = bridge;
    }

    /** True means own reducer accepted this snapshot; it is not a stock UI/game consumption flag. */
    public synchronized boolean capture(MotionEvent event) {
        if (event == null) { bridge.reset(); return false; }
        if ((event.getSource() & InputDevice.SOURCE_TOUCHSCREEN) != InputDevice.SOURCE_TOUCHSCREEN) return false;
        int action = event.getActionMasked();
        if (action == MotionEvent.ACTION_CANCEL || action == MotionEvent.ACTION_OUTSIDE)
            return bridge.feed(action, -1, null, null, null, 0);
        if (action != MotionEvent.ACTION_DOWN && action != MotionEvent.ACTION_UP && action != MotionEvent.ACTION_MOVE
                && action != MotionEvent.ACTION_POINTER_DOWN && action != MotionEvent.ACTION_POINTER_UP) return false;
        int count = event.getPointerCount();
        int index = event.getActionIndex();
        if (count <= 0 || count > MAX_POINTERS || (action != MotionEvent.ACTION_MOVE && (index < 0 || index >= count))) {
            bridge.reset(); return false;
        }
        for (int i = 0; i < count; ++i) {
            ids[i] = event.getPointerId(i);
            xs[i] = event.getX(i);
            ys[i] = event.getY(i);
        }
        int changed = action == MotionEvent.ACTION_MOVE ? -1 : ids[index];
        return bridge.feed(action, changed, ids, xs, ys, count);
    }

    public void onFocusChanged(boolean focused) { bridge.setFocused(focused); }
    public void onPaused(boolean paused) { bridge.setPaused(paused); }
    public void onImeVisibilityChanged(boolean visible) { bridge.setImeVisible(visible); }
    public void onBackOrUiCapture() { bridge.reset(); }
    public void onSurfaceDestroyed() { bridge.invalidateLayout(); }
    public boolean onViewportChanged(float[] layout) { return bridge.configureLayout(layout); }
}
