package org.mta.mobile.frame;

import android.util.Log;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;

/** Called only by the one reviewed ExecutorThread.guardedRun invoke replacement. */
public final class FrameBridge {
    private static final String TAG = "MtaFrameBridge";
    private static final Method ORIGINAL = originalMethod();
    private static final Object LOCK = new Object();
    private static Thread owner;
    private static boolean loadAttempted;
    private static boolean telemetryAvailable;
    private static boolean failureReported;

    private FrameBridge() {}

    private static Method originalMethod() {
        try {
            // The original implIsInitialized call precedes this shim in the APK.
            // Initializing this class also retains Game's own loadLibrary ordering.
            Class<?> game = Class.forName("com.rockstargames.oswrapper.GameNative");
            Method draw = game.getMethod("implOnDrawFrame", float.class);
            int flags = draw.getModifiers();
            if (!Modifier.isPublic(flags) || !Modifier.isStatic(flags) || !Modifier.isNative(flags)
                    || draw.getReturnType() != void.class) {
                throw new NoSuchMethodException("original public static native draw descriptor changed");
            }
            return draw;
        } catch (ReflectiveOperationException error) {
            throw new ExceptionInInitializerError(error);
        }
    }

    private static void disable(String reason, Throwable error) {
        synchronized (LOCK) {
            telemetryAvailable = false;
            if (!failureReported) {
                failureReported = true;
                Log.e(TAG, reason, error);
            }
        }
    }

    private static long before(float delta) {
        synchronized (LOCK) {
            Thread current = Thread.currentThread();
            if (owner == null) owner = current;
            if (owner != current) {
                disable("telemetry disabled: Executor frame arrived from a different Thread", null);
                return 0;
            }
            if (!loadAttempted) {
                loadAttempted = true;
                try {
                    System.loadLibrary("mta_frame_bridge");
                    telemetryAvailable = true;
                } catch (LinkageError error) {
                    disable("own JNI unavailable; original native draw retained", error);
                }
            }
            if (!telemetryAvailable) return 0;
            try {
                return nativeBefore(current.getId(), delta);
            } catch (RuntimeException | LinkageError error) {
                disable("own JNI before-frame failed; original native draw retained", error);
                return 0;
            }
        }
    }

    private static void after(long ticket, boolean completed) {
        if (ticket == 0) return;
        synchronized (LOCK) {
            if (!telemetryAvailable) return;
            try {
                nativeAfter(ticket, completed);
            } catch (RuntimeException | LinkageError error) {
                disable("own JNI after-frame failed; original draw outcome retained", error);
            }
        }
    }

    private static void disableQuietly(String reason, Throwable error) {
        try {
            disable(reason, error);
        } catch (Throwable ignored) {
            // Even diagnostics/logging failure must not replace the game's draw.
        }
    }

    /** Same (F)V descriptor and original exception object; no second game draw. */
    public static void draw(float delta) {
        long ticket = 0;
        try {
            ticket = before(delta);
        } catch (Throwable error) {
            disableQuietly("own telemetry before-frame failed; original draw retained", error);
        }
        boolean completed = false;
        try {
            ORIGINAL.invoke(null, delta);
            completed = true;
        } catch (InvocationTargetException error) {
            throw FrameBridge.<RuntimeException>rethrow(error.getCause());
        } catch (IllegalAccessException error) {
            throw new IllegalStateException("original public draw unexpectedly inaccessible", error);
        } finally {
            try {
                after(ticket, completed);
            } catch (Throwable error) {
                disableQuietly("own telemetry after-frame failed; original draw outcome retained", error);
            }
        }
    }

    @SuppressWarnings("unchecked")
    private static <E extends Throwable> RuntimeException rethrow(Throwable error) throws E {
        // Reflection wraps native exceptions; preserve the original Throwable,
        // including an unusual checked exception thrown by JNI.
        throw (E) error;
    }

    private static native long nativeBefore(long javaThreadId, float delta);
    private static native void nativeAfter(long ticket, boolean completed);
}
