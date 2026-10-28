package org.mta.mobile.input;

/** Own JNI state only: no game engine calls, View ownership or Android UI dependencies. */
public final class NativeInputBridge implements AutoCloseable {
    static { System.loadLibrary("mta_input_bridge"); }
    private long handle;

    public NativeInputBridge() {
        handle = nativeCreate();
        if (handle == 0) throw new IllegalStateException("Native input context limit or allocation failure");
    }

    /** width,height; safe/movement/look/action rectangles (4 each); radius,deadzone. */
    public synchronized boolean configureLayout(float[] layout) { return nativeConfigure(handle, layout); }
    public synchronized void invalidateLayout() { nativeConfigure(handle, null); }
    public synchronized boolean feed(int actionMasked, int changedId, int[] ids, float[] xs, float[] ys, int count) {
        return nativeFeed(handle, actionMasked, changedId, ids, xs, ys, count);
    }
    public synchronized boolean setFocused(boolean focused) { return nativeGate(handle, 0, focused); }
    public synchronized boolean setPaused(boolean paused) { return nativeGate(handle, 1, paused); }
    public synchronized boolean setImeVisible(boolean visible) { return nativeGate(handle, 2, visible); }
    public synchronized boolean reset() { return nativeReset(handle); }
    /** Caller owns the reusable frame; use a single frame consumer on the game thread. */
    public synchronized boolean consume(Frame frame) {
        if (frame == null) throw new NullPointerException("frame");
        boolean accepted = false;
        try { return accepted = nativeConsume(handle, frame.axes, frame.metadata); }
        finally { if (!accepted) frame.clear(); }
    }
    @Override public synchronized void close() {
        long previous = handle;
        handle = 0;
        if (previous != 0) nativeDestroy(previous);
    }

    public static final class Frame {
        public final float[] axes = new float[4]; // moveX/Y, one-shot look deltaX/Y
        public final long[] metadata = new long[4]; // action,enabled,epoch,sequence
        public boolean action() { return metadata[0] != 0; }
        public boolean enabled() { return metadata[1] != 0; }
        public long epoch() { return metadata[2]; }
        public long sequence() { return metadata[3]; }
        private void clear() {
            for (int i = 0; i < axes.length; ++i) axes[i] = 0;
            for (int i = 0; i < metadata.length; ++i) metadata[i] = 0;
        }
    }

    private static native long nativeCreate();
    private static native void nativeDestroy(long handle);
    private static native boolean nativeConfigure(long handle, float[] layout);
    private static native boolean nativeFeed(long handle, int actionMasked, int changedId, int[] ids, float[] xs, float[] ys, int count);
    private static native boolean nativeGate(long handle, int gate, boolean enabled);
    private static native boolean nativeReset(long handle);
    private static native boolean nativeConsume(long handle, float[] axes, long[] metadata);
}
