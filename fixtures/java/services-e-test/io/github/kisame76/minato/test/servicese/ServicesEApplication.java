package io.github.kisame76.minato.test.servicese;

import android.app.Application;
import android.content.Context;
import android.content.Intent;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import java.lang.reflect.Constructor;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.util.Arrays;
import java.util.Collection;
import java.util.concurrent.Callable;

/**
 * Single answers that apps ask for: the service names dropbox, tv_input and content_capture by name and by class with
 * a few harmless calls, a view's content capture session, MediaDrm for two schemes, and the subscription slot of
 * subscription ids, each logged with its result, or with the class of the exception it throws. The classes and methods
 * are reached by name, so that the same sources build against Android's SDK and against Minato's framework.
 */
public class ServicesEApplication extends Application {
    private static final String TAG = "services-e-test";

    private static void log(String name, Callable<Object> call) {
        log(TAG, name, call);
    }

    private static void log(String tag, String name, Callable<Object> call) {
        String result;
        try {
            final Object value = call.call();
            result = value instanceof Collection ? "size " + ((Collection<?>) value).size() : String.valueOf(value);
        } catch (InvocationTargetException e) {
            result = "throws " + e.getCause().getClass().getName();
        } catch (Throwable e) {
            result = "throws " + e.getClass().getName();
        }
        Log.i(tag, name + ": " + result);
    }

    /** Whether the method takes these arguments: a null fits any reference type, so overloads are told apart. */
    private static boolean accepts(Method candidate, Object[] args) {
        final Class<?>[] types = candidate.getParameterTypes();
        if (types.length != args.length) return false;
        for (int i = 0; i < types.length; i++) {
            if (args[i] == null) {
                if (types[i].isPrimitive()) return false;
            } else if (types[i].isPrimitive()) {
                if (types[i] == int.class && !(args[i] instanceof Integer)) return false;
            } else if (!types[i].isInstance(args[i])) {
                return false;
            }
        }
        return true;
    }

    private static Object call(Object target, String method, Object... args) throws Exception {
        for (Method candidate : target.getClass().getMethods()) {
            if (candidate.getName().equals(method) && accepts(candidate, args)) {
                return candidate.invoke(target, args);
            }
        }
        throw new NoSuchMethodException(target.getClass().getName() + "." + method);
    }

    private static Object callStatic(String type, String method, Object... args) throws Exception {
        for (Method candidate : Class.forName(type).getMethods()) {
            if (candidate.getName().equals(method) && accepts(candidate, args)) {
                return candidate.invoke(null, args);
            }
        }
        throw new NoSuchMethodException(type + "." + method);
    }

    private static Object construct(String type, Class<?> parameter, Object argument) throws Exception {
        final Constructor<?> constructor = parameter == null ? Class.forName(type).getConstructor()
                : Class.forName(type).getConstructor(parameter);
        return parameter == null ? constructor.newInstance() : constructor.newInstance(argument);
    }

    private static String short_(String type) {
        return type.substring(type.lastIndexOf('.') + 1);
    }

    private Object present(String name, String type) {
        final Object byName = getSystemService(name);
        Object byClass = null;
        try {
            byClass = getSystemService(Class.forName(type));
        } catch (ClassNotFoundException e) {
            Log.i(TAG, short_(type) + " class: missing");
        }
        Log.i(TAG, short_(type) + " by name: " + (byName != null) + ", by class: " + (byClass != null)
                + ", same: " + (byName == byClass));
        return byName;
    }

    @Override
    public void onCreate() {
        super.onCreate();
        new Handler(Looper.getMainLooper()).post(this::run);
    }

    private void run() {
        final Object dropbox = present(Context.DROPBOX_SERVICE, "android.os.DropBoxManager");
        if (dropbox != null) {
            log("DropBoxManager.isTagEnabled(minato)", () -> call(dropbox, "isTagEnabled", "minato"));
            log("DropBoxManager.getNextEntry(minato, 0)", () -> call(dropbox, "getNextEntry", "minato", 0L));
        }
        final Object tv = present(Context.TV_INPUT_SERVICE, "android.media.tv.TvInputManager");
        if (tv != null) {
            log("TvInputManager.getTvInputList()", () -> call(tv, "getTvInputList"));
            log("TvInputManager.getInputState(none)", () -> call(tv, "getInputState", "none"));
        }
        final Object capture = present("content_capture",
                "android.view.contentcapture.ContentCaptureManager");
        if (capture != null) {
            log("ContentCaptureManager.isContentCaptureEnabled()", () -> call(capture, "isContentCaptureEnabled"));
        }
        log("View.getContentCaptureSession() is null", () -> new android.view.View(this)
                .getContentCaptureSession() == null);
        log("View.getImportantForContentCapture()", () -> new android.view.View(this)
                .getImportantForContentCapture());

        final String[] schemes = {"edef8ba9-79d6-4ace-a3c8-27dcd51d21ed", "e2719d58-a985-b3c9-781a-b030af78d30e",
                "00000000-0000-0000-0000-000000000000"};
        for (final String scheme : schemes) {
            final java.util.UUID uuid = java.util.UUID.fromString(scheme);
            // The emulator has a ClearKey plugin, which Minato does not (no DRM plugin runs on the iPhone): its lines
            // are only shown, the others (no plugin: false, UnsupportedSchemeException) are compared.
            final String tag = scheme.startsWith("e271") ? TAG + "-device" : TAG;
            log(tag, "MediaDrm.isCryptoSchemeSupported(" + scheme.substring(0, 4) + ")",
                    () -> callStatic("android.media.MediaDrm", "isCryptoSchemeSupported", uuid));
            log(tag, "MediaDrm(" + scheme.substring(0, 4) + ") then release()", () -> {
                final Object drm = construct("android.media.MediaDrm", java.util.UUID.class, uuid);
                call(drm, "release");
                return "released";
            });
        }
        for (final int id : new int[] {-1, 0, 1, 2, Integer.MAX_VALUE}) {
            log("SubscriptionManager.getSlotIndex(" + id + ")",
                    () -> callStatic("android.telephony.SubscriptionManager", "getSlotIndex", id));
        }
        Log.i(TAG, "done");
    }
}
