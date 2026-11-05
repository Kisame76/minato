package io.github.kisame76.minato.test.servicesc;

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
 * Six classes that apps ask at start: the fingerprint, grammatical inflection and print managers by name and by class,
 * and the share action provider, the quick settings tile service and the DRM manager client by their constructors and
 * a few harmless calls, each logged with its result, or with the class of the exception it throws. The app holds none
 * of their permissions, so what needs one shows Android's SecurityException. The classes and methods are reached by
 * name, so that the same sources build against Android's SDK and against Minato's framework before it has the classes.
 */
public class ServicesCApplication extends Application {
    private static final String TAG = "services-c-test";

    private static void log(String name, Callable<Object> call) {
        String result;
        try {
            final Object value = call.call();
            result = value instanceof Collection ? "size " + ((Collection<?>) value).size() : String.valueOf(value);
        } catch (InvocationTargetException e) {
            result = "throws " + e.getCause().getClass().getName();
        } catch (Throwable e) {
            result = "throws " + e.getClass().getName();
        }
        Log.i(TAG, name + ": " + result);
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
        final Context context = this;
        final Object fingerprint = present(Context.FINGERPRINT_SERVICE,
                "android.hardware.fingerprint.FingerprintManager");
        final Object inflection = present(Context.GRAMMATICAL_INFLECTION_SERVICE,
                "android.app.GrammaticalInflectionManager");
        final Object print = present(Context.PRINT_SERVICE, "android.print.PrintManager");

        if (fingerprint != null) {
            log("FingerprintManager.isHardwareDetected()", () -> call(fingerprint, "isHardwareDetected"));
            log("FingerprintManager.hasEnrolledFingerprints()", () -> call(fingerprint, "hasEnrolledFingerprints"));
        }
        final String crypto = "android.hardware.fingerprint.FingerprintManager$CryptoObject";
        log("CryptoObject(Cipher).getCipher() is the cipher", () -> {
            final javax.crypto.Cipher cipher = javax.crypto.Cipher.getInstance("AES/CBC/PKCS5Padding");
            return call(construct(crypto, javax.crypto.Cipher.class, cipher), "getCipher") == cipher;
        });
        log("CryptoObject(Cipher).getSignature() is null", () -> call(construct(crypto, javax.crypto.Cipher.class,
                javax.crypto.Cipher.getInstance("AES/CBC/PKCS5Padding")), "getSignature") == null);
        log("AuthenticationCallback is abstract", () -> Modifier.isAbstract(Class.forName(
                "android.hardware.fingerprint.FingerprintManager$AuthenticationCallback").getModifiers()));
        log("AuthenticationCallback methods", () -> {
            final Method[] methods = Class.forName(
                    "android.hardware.fingerprint.FingerprintManager$AuthenticationCallback").getDeclaredMethods();
            final String[] names = new String[methods.length];
            for (int i = 0; i < methods.length; i++) names[i] = methods[i].getName();
            Arrays.sort(names);
            return Arrays.toString(names);
        });

        if (inflection != null) {
            log("GrammaticalInflectionManager.getApplicationGrammaticalGender()",
                    () -> call(inflection, "getApplicationGrammaticalGender"));
            log("GrammaticalInflectionManager.getSystemGrammaticalGender()",
                    () -> call(inflection, "getSystemGrammaticalGender"));
            log("GrammaticalInflectionManager.setRequestedApplicationGrammaticalGender(0)",
                    () -> call(inflection, "setRequestedApplicationGrammaticalGender", 0));
        }

        if (print != null) {
            log("PrintManager.print(null adapter)", () -> call(print, "print", "job", null, null));
        }

        log("ShareActionProvider(context).hasSubMenu()", () -> call(
                construct("android.widget.ShareActionProvider", Context.class, context), "hasSubMenu"));
        log("ShareActionProvider(context).onCreateActionView() is not null", () -> call(
                construct("android.widget.ShareActionProvider", Context.class, context), "onCreateActionView") != null);
        log("ShareActionProvider.setShareIntent(intent)", () -> call(
                construct("android.widget.ShareActionProvider", Context.class, context), "setShareIntent",
                new Intent(Intent.ACTION_SEND)));

        log("TileService extends android.app.Service", () -> Class.forName("android.service.quicksettings.TileService")
                .getSuperclass().getName());
        log("TileService.ACTION_QS_TILE", () -> Class.forName("android.service.quicksettings.TileService")
                .getField("ACTION_QS_TILE").get(null));
        log("TileService().getQsTile() is null", () -> call(construct("android.service.quicksettings.TileService",
                null, null), "getQsTile") == null);
        log("TileService().isLocked()", () -> call(construct("android.service.quicksettings.TileService", null, null),
                "isLocked"));
        log("TileService().isSecure()", () -> call(construct("android.service.quicksettings.TileService", null, null),
                "isSecure"));

        final String drm = "android.drm.DrmManagerClient";
        log("DrmManagerClient(context).canHandle(path, mime)", () -> {
            final Object client = construct(drm, Context.class, context);
            try {
                return call(client, "canHandle", "/data/local/tmp/none.dcf", "application/vnd.oma.drm.content");
            } finally {
                call(client, "release");
            }
        });
        log("DrmManagerClient(context).getAvailableDrmEngines().length", () -> {
            final Object client = construct(drm, Context.class, context);
            try {
                return ((Object[]) call(client, "getAvailableDrmEngines")).length;
            } finally {
                call(client, "release");
            }
        });
        log("DrmManagerClient(context).getDrmObjectType(path, mime)", () -> {
            final Object client = construct(drm, Context.class, context);
            try {
                return call(client, "getDrmObjectType", "/data/local/tmp/none.dcf", "video/mp4");
            } finally {
                call(client, "release");
            }
        });
        log("DrmStore.Action.PLAY", () -> Class.forName("android.drm.DrmStore$Action").getField("PLAY").get(null));
        Log.i(TAG, "done");
    }
}
