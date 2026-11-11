package io.github.kisame76.minato.test.hiddenapi;

import android.app.Application;
import android.os.StrictMode;
import android.util.Log;

import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;

/**
 * Android's hidden API policy as an app meets it (the hidden API plan): members of the platform's classes that are
 * hidden, each on one of the lists of Android 15's hiddenapi-flags.csv, looked up by reflection with getDeclaredField,
 * getDeclaredMethod or getDeclaredConstructor and searched for in the class's getDeclaredFields, getDeclaredMethods
 * or getDeclaredConstructors; and the lines StrictMode.detectNonSdkApiUsage reports as the app does it. One app from
 * one source for three targets: hidden-api-test for 35, hidden-api-test-33 and hidden-api-test-28, whose lists
 * the target SDK sets (max-target-p to -s allow an app only up to their level; unsupported always; blocked never).
 * An app is denied what Android denies it: "NoSuchFieldException" or "NoSuchMethodException", and the member not in
 * the class's list.
 */
public class HiddenApiApplication extends Application {
    private String tag;

    // {list, kind (F field, M method, C constructor), class, member name, parameter types...}
    private static final String[][] PROBES = {
        {"sdk", "M", "android.app.Activity", "onCreate", "android.os.Bundle"},
        {"unsupported", "F", "android.os.Handler", "mCallback"},
        {"unsupported", "M", "android.os.Handler", "getIMessenger"},
        {"unsupported", "C", "android.os.Handler", "<init>", "android.os.Looper", "android.os.Handler$Callback",
            "boolean"},
        {"max-target-o", "F", "android.app.Activity$ManagedCursor", "mCursor"},
        {"max-target-p", "F", "android.app.Activity", "mVisibleFromClient"},
        {"max-target-p", "M", "android.app.Activity", "setParent", "android.app.Activity"},
        {"max-target-q", "F", "android.app.Activity", "mParent"},
        {"max-target-r", "F", "android.app.Activity", "FRAGMENTS_TAG"},
        {"max-target-s", "M", "android.app.Activity", "setDisablePreviewScreenshots", "boolean"},
        {"blocked", "F", "android.app.Activity", "DUMP_ARG_AUTOFILL"},
        {"blocked+test-api", "M", "android.app.Activity", "addDumpable", "android.util.Dumpable"},
        {"none", "M", "android.app.Activity", "noSuchMethod"},
    };

    @Override
    public void onCreate() {
        super.onCreate();
        int target = getApplicationInfo().targetSdkVersion;
        tag = target >= 35 ? "hidden-api-test" : target >= 33 ? "hidden-api-test-33" : "hidden-api-test-28";
        Log.i(tag, "target SDK " + target);
        StrictMode.setVmPolicy(new StrictMode.VmPolicy.Builder().detectNonSdkApiUsage()
            .penaltyListener(Runnable::run, violation -> Log.i(tag, "StrictMode: " + violation.getMessage())).build());
        for (String[] probe : PROBES) {
            try {
                probe(probe);
            } catch (Throwable thrown) {
                Log.i(tag, probe[0] + " " + probe[2] + "." + probe[3] + ": " + thrown);
            }
        }
        Log.i(tag, "done");
    }

    private void probe(String[] probe) throws Exception {
        Class<?> owner = Class.forName(probe[2]);
        Class<?>[] types = new Class<?>[probe.length - 4];
        for (int i = 0; i < types.length; i++) {
            types[i] = forName(probe[4 + i]);
        }
        String name = probe[3];
        String found;
        boolean listed = false;
        try {
            switch (probe[1]) {
                case "F":
                    owner.getDeclaredField(name);
                    break;
                case "C":
                    owner.getDeclaredConstructor(types);
                    break;
                default:
                    owner.getDeclaredMethod(name, types);
            }
            found = "found";
        } catch (NoSuchFieldException | NoSuchMethodException missing) {
            found = missing.getClass().getSimpleName();
        }
        switch (probe[1]) {
            case "F":
                for (Field field : owner.getDeclaredFields()) {
                    listed |= field.getName().equals(name);
                }
                break;
            case "C":
                for (Constructor<?> constructor : owner.getDeclaredConstructors()) {
                    listed |= constructor.getParameterCount() == types.length;
                }
                break;
            default:
                for (Method method : owner.getDeclaredMethods()) {
                    listed |= method.getName().equals(name);
                }
        }
        Log.i(tag, probe[0] + " " + owner.getName() + "." + name + ": " + found + ", "
            + (listed ? "listed" : "not listed"));
    }

    private static Class<?> forName(String name) throws ClassNotFoundException {
        switch (name) {
            case "boolean":
                return boolean.class;
            case "int":
                return int.class;
            default:
                return Class.forName(name);
        }
    }
}
