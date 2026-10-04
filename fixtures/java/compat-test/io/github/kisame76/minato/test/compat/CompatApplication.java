package io.github.kisame76.minato.test.compat;

import android.app.Application;
import android.content.SharedPreferences;
import android.os.IBinder;
import android.util.Log;
import android.util.SparseIntArray;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.lang.reflect.InvocationTargetException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.UUID;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;
import java.util.zip.ZipOutputStream;

/**
 * ADR-0039 step c2e's test, built twice from these sources: compat-test for targetSdk 28 and compat-test-35 for 35, so
 * that each change shows on both sides of its target. In Application.onCreate, as there is no activity before step
 * c2c3, it logs with the build's name as its tag:
 * <ul>
 *   <li>"abc".split(""): libcore's Pattern reads ART's target SDK itself, and keeps the leading empty string up to 28;
 *   <li>UUID.fromString of 37 characters: ENABLE_STRICT_VALIDATION, from 34 on, through libcore's Compatibility;
 *   <li>SharedPreferences' listener at clear(): CALLBACK_ON_CLEAR_CHANGE, from 30 on, a call with a null key;
 *   <li>SparseIntArray.valueAt past its size, within its array: UtilConfig's check from 29 on, which
 *       handleBindApplication sets;
 *   <li>a zip entry "../escape.txt" through ZipInputStream: the zip path validator from 34 on;
 *   <li>the platform compat service, which an app may not ask: Android's hidden API policy stops the reflection on the
 *       AVD, and the service's permission check on Minato, so it goes to the device tag, which tools/compat-check.sh
 *       checks on the Mac.
 * </ul>
 */
public class CompatApplication extends Application {
    /** UUID's ENABLE_STRICT_VALIDATION, about which the platform compat service is asked. */
    private static final long ENABLE_STRICT_VALIDATION = 263076149L;

    private String tag;

    private interface Section {
        void run() throws Exception;
    }

    @Override
    public void onCreate() {
        super.onCreate();
        int target = getApplicationInfo().targetSdkVersion;
        tag = target >= 35 ? "compat-test-35" : "compat-test";
        Log.i(tag, "target SDK " + target);
        guarded(tag, "\"abc\".split(\"\")", () -> Log.i(tag, "\"abc\".split(\"\"): "
                + Arrays.toString("abc".split(""))));
        guarded(tag, "UUID.fromString of 37 characters", () -> Log.i(tag, "UUID.fromString of 37 characters: "
                + UUID.fromString("00000000-0000-0000-0000-0000000000001")));
        guarded(tag, "SharedPreferences.clear()", this::clearPreferences);
        guarded(tag, "new SparseIntArray(1).valueAt(0)", () -> Log.i(tag, "new SparseIntArray(1).valueAt(0): "
                + new SparseIntArray(1).valueAt(0)));
        guarded(tag, "ZipInputStream of \"../escape.txt\"", this::readEscapingEntry);
        guarded(tag + "-device", "platform_compat", this::askPlatformCompat);
        Log.i(tag, "done");
    }

    private void clearPreferences() {
        SharedPreferences preferences = getSharedPreferences("compat", MODE_PRIVATE);
        preferences.edit().clear().commit();
        preferences.edit().putString("key", "value").commit();
        final List<String> keys = new ArrayList<>();
        SharedPreferences.OnSharedPreferenceChangeListener listener = (changed, key) -> keys.add(key);
        preferences.registerOnSharedPreferenceChangeListener(listener);
        preferences.edit().clear().commit();
        preferences.unregisterOnSharedPreferenceChangeListener(listener);
        Log.i(tag, "SharedPreferences.clear(): the listener got " + keys);
    }

    private void readEscapingEntry() throws Exception {
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        try (ZipOutputStream out = new ZipOutputStream(bytes)) {
            out.putNextEntry(new ZipEntry("../escape.txt"));
            out.write('x');
            out.closeEntry();
        }
        try (ZipInputStream in = new ZipInputStream(new ByteArrayInputStream(bytes.toByteArray()))) {
            Log.i(tag, "ZipInputStream of \"../escape.txt\": " + in.getNextEntry().getName());
        }
    }

    private void askPlatformCompat() throws Exception {
        IBinder binder = (IBinder) Class.forName("android.os.ServiceManager").getMethod("getService", String.class)
                .invoke(null, "platform_compat");
        Object service = Class.forName("com.android.internal.compat.IPlatformCompat$Stub")
                .getMethod("asInterface", IBinder.class).invoke(null, binder);
        Object enabled = Class.forName("com.android.internal.compat.IPlatformCompat")
                .getMethod("isChangeEnabledByPackageName", long.class, String.class, int.class)
                .invoke(service, ENABLE_STRICT_VALIDATION, getPackageName(), 0);
        Log.i(tag + "-device", "platform_compat: " + enabled);
    }

    /** Runs a section; what it throws is logged with its name, the cause of a reflective call's exception unwrapped. */
    private static void guarded(String tag, String name, Section section) {
        try {
            section.run();
        } catch (Exception e) {
            Throwable thrown = e instanceof InvocationTargetException ? e.getCause() : e;
            Log.i(tag, name + ": " + thrown);
        }
    }
}
