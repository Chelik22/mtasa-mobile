package org.mta.mobile.input;

/** Runs the real JNI library from a real host JVM; never fabricates Android event delivery. */
public final class BridgeJvmTrace {
    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }
    private static void near(float actual, float expected, String message) {
        check(Math.abs(actual - expected) < 0.0001f, message);
    }
    private static float[] layout() {
        return new float[] {1000,1000, 100,100,900,900, 0,0,.45f,1, .55f,0,1,.7f,
                .55f,.75f,1,1, .25f,.2f};
    }
    private static void ready(NativeInputBridge bridge) {
        check(bridge.configureLayout(layout()), "configure");
        check(bridge.setFocused(true) && bridge.setPaused(false), "focus/resume");
    }
    private static void neutral(NativeInputBridge.Frame frame) {
        for (float axis : frame.axes) near(axis, 0, "stuck axis");
        check(!frame.action(), "stuck action");
    }
    private static void traces() {
        NativeInputBridge bridge = new NativeInputBridge();
        NativeInputBridge.Frame frame = new NativeInputBridge.Frame();
        try {
            check(bridge.consume(frame) && !frame.enabled(), "initial lifecycle must gate input");
            ready(bridge);
            check(bridge.feed(0, 7, new int[]{7}, new float[]{260}, new float[]{500}, 1), "begin");
            check(bridge.feed(5, 42, new int[]{7,42}, new float[]{260,740}, new float[]{500,260}, 2), "look down");
            check(bridge.feed(5, 3, new int[]{3,42,7}, new float[]{740,740,260}, new float[]{780,260,500}, 3), "action down");
            check(bridge.feed(2, -1, new int[]{42,7,3}, new float[]{660,380,740}, new float[]{340,500,780}, 3), "reordered move");
            check(bridge.consume(frame), "consume");
            near(frame.axes[0], .5f, "movement"); near(frame.axes[2], -.1f, "camera x"); near(frame.axes[3], .1f, "camera y");
            check(frame.action() && frame.enabled(), "independent action");
            long sequence = frame.sequence();
            check(bridge.consume(frame), "second consume"); near(frame.axes[2],0,"delta repeated"); near(frame.axes[3],0,"delta repeated");
            check(frame.action() && frame.sequence() > sequence, "held action/sequence");
            check(bridge.feed(6, 42, new int[]{3,7,42}, new float[]{740,380,660}, new float[]{780,500,340}, 3), "look up");
            check(bridge.feed(2, -1, new int[]{3,7}, new float[]{740,420}, new float[]{780,500}, 2), "reorder after up");
            check(bridge.consume(frame), "consume after up"); near(frame.axes[0],.75f,"pointer ID ownership");
            check(frame.action(), "lifting look released action");
            long epoch = frame.epoch();
            check(bridge.setImeVisible(true) && bridge.consume(frame), "IME"); neutral(frame);
            check(!frame.enabled() && frame.epoch()>epoch,"IME gate/epoch");
            check(bridge.setImeVisible(false) && bridge.setFocused(false) && bridge.consume(frame), "focus"); neutral(frame);
            check(!frame.enabled(), "focus gate");
            check(bridge.setFocused(true) && bridge.setPaused(true) && bridge.consume(frame), "pause"); neutral(frame);
            check(!frame.enabled(), "pause gate");
            check(bridge.setPaused(false), "resume");
            check(bridge.feed(2, -1, new int[]{7}, new float[]{420}, new float[]{500}, 1), "stale move");
            check(bridge.consume(frame), "postresume"); neutral(frame);
            check(!bridge.configureLayout(new float[19]) && bridge.consume(frame), "invalid layout rejected");
            check(!frame.enabled(), "invalid layout retained old viewport");
            ready(bridge);
            check(bridge.feed(3, -1, null, null, null, 0) && bridge.consume(frame), "cancel empty snapshot"); neutral(frame);
            check(!bridge.feed(0, 1, new int[]{1}, new float[]{0}, new float[]{0}, 2), "short arrays accepted");
            check(!bridge.feed(2, -1, new int[]{1,1}, new float[]{260,740}, new float[]{500,260}, 2), "duplicate IDs accepted");
            check(!bridge.feed(2, -1, new int[]{1}, new float[]{Float.NaN}, new float[]{500}, 1), "NaN accepted");
            check(bridge.consume(frame), "negative inputs readback"); neutral(frame);
            bridge.invalidateLayout(); check(bridge.consume(frame) && !frame.enabled(), "surface lost gate");
        } finally { bridge.close(); }
        bridge.close();
        check(!bridge.consume(frame) && !frame.enabled(), "closed handle accepted or stale enabled frame"); neutral(frame);
    }
    private static void limits() {
        NativeInputBridge[] contexts = new NativeInputBridge[16];
        try {
            for (int i=0;i<contexts.length;++i) contexts[i] = new NativeInputBridge();
            boolean rejected = false;
            try (NativeInputBridge excess = new NativeInputBridge()) { }
            catch (IllegalStateException expected) { rejected = true; }
            check(rejected,"context bound accepted");
        } finally { for (NativeInputBridge context:contexts) if (context!=null) context.close(); }
        try (NativeInputBridge reused = new NativeInputBridge()) {
            check(reused.reset(),"slot cleanup did not free capacity");
        }
    }
    public static void main(String[] args) {
        traces(); limits();
        System.out.println("PASS actual JVM/JNI scalar touch traces and context lifecycle; Android MotionEvent/game/U1 NOT RUN");
    }
}
