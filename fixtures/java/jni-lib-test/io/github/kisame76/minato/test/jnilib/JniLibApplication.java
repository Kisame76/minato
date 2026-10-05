package io.github.kisame76.minato.test.jnilib;

import android.app.Application;
import android.content.Context;
import android.util.Log;
import dalvik.system.BaseDexClassLoader;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.util.Arrays;

/**
 * ADR-0039 step c5.1's test: the app's JNI library where Android's installer puts it, in nativeLibraryDir, the code
 * path's lib/arm64. It logs, with the tag jni-lib-test, what the class loader's findLibrary answers, the folder's
 * files with their size, access and first bytes, and what the library answers after System.loadLibrary, and ends
 * with "done"; tools/android-reference.sh compares it with Android 15's log. No line names the code path, which
 * differs by install.
 */
public class JniLibApplication extends Application {
    private static final String TAG = "jni-lib-test";
    private static final String NAME = "jni-lib-test";

    /** The library's answer (jni/jni_lib_test.c). */
    static native String answer();

    @Override
    protected void attachBaseContext(Context base) {
        super.attachBaseContext(base);
        try {
            String folder = base.getApplicationInfo().nativeLibraryDir;
            String found = ((BaseDexClassLoader) JniLibApplication.class.getClassLoader()).findLibrary(NAME);
            String there = new File(folder, System.mapLibraryName(NAME)).getPath();
            Log.i(TAG, "findLibrary: " + (found == null ? "null" : found.equals(there) ? "in nativeLibraryDir" : "elsewhere"));
            String[] names = new File(folder).list();
            if (names == null) {
                Log.i(TAG, "nativeLibraryDir: no folder");
            } else {
                Arrays.sort(names);
                Log.i(TAG, "nativeLibraryDir: " + (names.length == 0 ? "empty" : String.join(" ", names)));
                for (String name : names) {
                    File file = new File(folder, name);
                    Log.i(TAG, name + ": " + file.length() + " bytes, readable " + file.canRead() + ", writable "
                            + file.canWrite() + ", begins " + begins(file));
                }
            }
            System.loadLibrary(NAME);
            Log.i(TAG, "loadLibrary: " + answer());
        } catch (Throwable t) {
            Log.i(TAG, "failed: " + t);
            // Its stack, which differs by device, is only shown.
            Log.i(TAG + "-device", Log.getStackTraceString(t));
        }
        Log.i(TAG, "done");
    }

    /** The file's first four bytes in hex, or why they could not be read. */
    private static String begins(File file) {
        byte[] bytes = new byte[4];
        try (FileInputStream in = new FileInputStream(file)) {
            int read = in.read(bytes);
            StringBuilder hex = new StringBuilder();
            for (int i = 0; i < read; i++) {
                hex.append(String.format("%02x", bytes[i]));
            }
            return hex.toString();
        } catch (IOException e) {
            return "unreadable";
        }
    }
}
