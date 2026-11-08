package io.github.kisame76.minato.test.biometric;

import android.app.Application;
import android.content.Context;
import android.content.pm.PackageManager;
import android.os.CancellationSignal;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import java.lang.reflect.Array;
import java.lang.reflect.Constructor;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collection;
import java.util.Collections;
import java.util.List;
import java.util.concurrent.Callable;
import java.util.concurrent.Executor;

/**
 * BiometricPrompt, BiometricManager and FingerprintManager on a device without a usable biometric: the prompt's builder
 * and getters, the crypto objects, canAuthenticate's answers and the error an authentication ends in, each logged with
 * its result, or with the class of the exception it throws. What depends on the device's own sensor (the emulator has an
 * emulated one, the iPhone none an Android app can use) is logged under the tag biometric-test-device, which is only
 * shown. The classes are reached by name, so that the same sources build against Android's SDK and Minato's framework.
 */
public class BiometricApplication extends Application {
    private static final String TAG = "biometric-test";
    private final Handler mHandler = new Handler(Looper.getMainLooper());

    private static void log(String name, Callable<Object> call) {
        log(TAG, name, call);
    }

    private static void log(String tag, String name, Callable<Object> call) {
        String result;
        try {
            final Object value = call.call();
            result = text(value);
        } catch (InvocationTargetException e) {
            result = "throws " + e.getCause().getClass().getName();
        } catch (Throwable e) {
            result = "throws " + e.getClass().getName();
        }
        Log.i(tag, name + ": " + result);
    }

    private static String text(Object value) {
        if (value == null) return "null";
        if (value.getClass().isArray()) {
            final List<String> parts = new ArrayList<>();
            for (int i = 0; i < Array.getLength(value); i++) parts.add(text(Array.get(value, i)));
            return parts.toString();
        }
        return value instanceof Collection ? "size " + ((Collection<?>) value).size() : String.valueOf(value);
    }

    /** Every public method without arguments that the class itself declares, by name, with its result. */
    private static void dump(String label, Object target, String... only) {
        final List<String> wanted = Arrays.asList(only);
        final List<Method> methods = new ArrayList<>(Arrays.asList(target.getClass().getDeclaredMethods()));
        Collections.sort(methods, (a, b) -> a.getName().compareTo(b.getName()));
        for (Method method : methods) {
            if (!Modifier.isPublic(method.getModifiers()) || method.getParameterTypes().length != 0
                    || method.isSynthetic() || Modifier.isStatic(method.getModifiers())) {
                continue;
            }
            final String name = method.getName();
            if (!wanted.contains(name)) continue;
            log(label + "." + name + "()", () -> method.invoke(target));
        }
    }

    /** The public static final int and String fields of a class, by name, as the constants an app compiles in. */
    private static void constants(String type, String... only) {
        final List<String> wanted = Arrays.asList(only);
        final StringBuilder out = new StringBuilder();
        try {
            final java.lang.reflect.Field[] fields = Class.forName(type).getDeclaredFields();
            Arrays.sort(fields, (a, b) -> a.getName().compareTo(b.getName()));
            for (java.lang.reflect.Field field : fields) {
                final int mods = field.getModifiers();
                if (wanted.contains(field.getName()) && Modifier.isPublic(mods) && Modifier.isStatic(mods)
                        && Modifier.isFinal(mods) && (field.getType() == int.class || field.getType() == String.class)) {
                    out.append(field.getName()).append('=').append(field.get(null)).append(' ');
                }
            }
        } catch (Throwable e) {
            out.append("throws ").append(e.getClass().getName());
        }
        Log.i(TAG, type.substring(type.lastIndexOf('.') + 1) + " constants: " + out.toString().trim());
    }

    private static boolean accepts(Method candidate, Object[] args) {
        final Class<?>[] types = candidate.getParameterTypes();
        if (types.length != args.length) return false;
        for (int i = 0; i < types.length; i++) {
            if (args[i] == null) {
                if (types[i].isPrimitive()) return false;
            } else if (types[i].isPrimitive()) {
                if (types[i] == int.class && !(args[i] instanceof Integer)) return false;
                if (types[i] == long.class && !(args[i] instanceof Long)) return false;
                if (types[i] == boolean.class && !(args[i] instanceof Boolean)) return false;
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

    private static Object construct(String type, Object... args) throws Exception {
        for (Constructor<?> candidate : Class.forName(type).getConstructors()) {
            final Class<?>[] types = candidate.getParameterTypes();
            boolean fits = types.length == args.length;
            for (int i = 0; fits && i < types.length; i++) {
                fits = args[i] == null ? !types[i].isPrimitive()
                        : types[i].isPrimitive() || types[i].isInstance(args[i]);
            }
            if (fits) return candidate.newInstance(args);
        }
        throw new NoSuchMethodException(type + ".<init>");
    }

    @Override
    public void onCreate() {
        super.onCreate();
        mHandler.post(this::run);
    }

    private void run() {
        final String manager = "android.hardware.biometrics.BiometricManager";
        constants(manager, "BIOMETRIC_ERROR_HW_UNAVAILABLE", "BIOMETRIC_ERROR_NONE_ENROLLED", "BIOMETRIC_ERROR_NO_HARDWARE", "BIOMETRIC_ERROR_SECURITY_UPDATE_REQUIRED", "BIOMETRIC_SUCCESS");
        final Object bm = getSystemService("biometric");
        Log.i(TAG, "BiometricManager by name: " + (bm != null));
        // Combinations the service does not take are a SecurityException on any device; the others' answers depend on
        // the device's sensor.
        for (final int authenticators : new int[] {0x7FFF, 0x1, 0x2}) {
            log("BiometricManager.canAuthenticate(" + Integer.toHexString(authenticators) + ")", () ->
                    call(bm, "canAuthenticate", authenticators));
        }
        log("BiometricManager.canAuthenticate(8000)", () -> call(bm, "canAuthenticate", 0x8000));
        for (final int authenticators : new int[] {0xFF, 0x80FF, 0xF, 0x800F}) {
            log(TAG + "-device", "BiometricManager.canAuthenticate(" + Integer.toHexString(authenticators) + ")", () ->
                    call(bm, "canAuthenticate", authenticators));
        }
        log(TAG + "-device", "BiometricManager.canAuthenticate()", () -> call(bm, "canAuthenticate"));
        final PackageManager packages = getPackageManager();
        log(TAG + "-device", "feature fingerprint", () -> packages.hasSystemFeature("android.hardware.fingerprint"));
        log(TAG + "-device", "feature face", () -> packages.hasSystemFeature("android.hardware.biometrics.face"));
        log(TAG + "-device", "feature iris", () -> packages.hasSystemFeature("android.hardware.biometrics.iris"));
        final String prompt = "android.hardware.biometrics.BiometricPrompt";
        constants(prompt, "AUTHENTICATION_RESULT_TYPE_BIOMETRIC", "AUTHENTICATION_RESULT_TYPE_DEVICE_CREDENTIAL");
        final Executor executor = mHandler::post;
        log("BiometricPrompt.Builder without title", () -> {
            final Object builder = construct(prompt + "$Builder", this);
            call(builder, "setNegativeButton", "Cancel", executor,
                    (android.content.DialogInterface.OnClickListener) (d, w) -> { });
            call(builder, "build");
            return "built";
        });
        log("BiometricPrompt.Builder without negative button", () -> {
            final Object builder = construct(prompt + "$Builder", this);
            call(builder, "setTitle", "Title");
            call(builder, "build");
            return "built";
        });
        log("BiometricPrompt.Builder set and built", () -> {
            final Object builder = construct(prompt + "$Builder", this);
            call(builder, "setTitle", "Title");
            call(builder, "setSubtitle", "Subtitle");
            call(builder, "setDescription", "Description");
            call(builder, "setNegativeButton", "Cancel", executor,
                    (android.content.DialogInterface.OnClickListener) (d, w) -> { });
            call(builder, "setConfirmationRequired", false);
            call(builder, "setAllowedAuthenticators", 0x0FF);
            final Object built = call(builder, "build");
            dump("BiometricPrompt", built, "getAllowedAuthenticators", "getContentView", "getDescription", "getLogoBitmap", "getLogoDescription", "getLogoRes", "getNegativeButtonText", "getSubtitle", "getTitle", "isConfirmationRequired");
            return built.getClass().getName();
        });
        log("BiometricPrompt.CryptoObject(Signature)", () -> {
            final Object crypto = construct(prompt + "$CryptoObject",
                    java.security.Signature.getInstance("SHA256withRSA"));
            return call(crypto, "getSignature") != null && call(crypto, "getCipher") == null;
        });
        log("BiometricPrompt.CryptoObject(Cipher)", () -> {
            final Object crypto = construct(prompt + "$CryptoObject",
                    javax.crypto.Cipher.getInstance("AES/CBC/PKCS5Padding"));
            return call(crypto, "getCipher") != null && call(crypto, "getMac") == null;
        });
        log("BiometricPrompt.CryptoObject(Mac)", () -> {
            final Object crypto = construct(prompt + "$CryptoObject", javax.crypto.Mac.getInstance("HmacSHA256"));
            return call(crypto, "getMac") != null;
        });
        mHandler.postDelayed(() -> authenticate(prompt, executor), 100);
        mHandler.postDelayed(this::fingerprint, 700);
        mHandler.postDelayed(() -> Log.i(TAG, "done"), 1500);
    }

    private void authenticate(String prompt, Executor executor) {
        try {
            final Object builder = construct(prompt + "$Builder", this);
            call(builder, "setTitle", "Title");
            call(builder, "setNegativeButton", "Cancel", executor,
                    (android.content.DialogInterface.OnClickListener) (d, w) -> { });
            final Object built = call(builder, "build");
            call(built, "authenticate", new CancellationSignal(), executor, new BiometricLogger());
            Log.i(TAG, "BiometricPrompt.authenticate returned");
        } catch (Throwable e) {
            Log.i(TAG, "BiometricPrompt.authenticate: throws " + (e instanceof InvocationTargetException
                    ? e.getCause().getClass().getName() : e.getClass().getName()));
        }
    }

    private void fingerprint() {
        final Object fm = getSystemService("fingerprint");
        log("FingerprintManager by name", () -> fm != null);
        if (fm != null) {
            log(TAG + "-device", "FingerprintManager.isHardwareDetected()", () -> call(fm, "isHardwareDetected"));
            log("FingerprintManager.hasEnrolledFingerprints()", () -> call(fm, "hasEnrolledFingerprints"));
            try {
                new FingerprintLogger().authenticate(fm);
            } catch (Throwable e) {
                Log.i(TAG, "FingerprintManager.authenticate: throws " + e.getClass().getName());
            }
        }
    }
}
