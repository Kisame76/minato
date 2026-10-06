package io.github.kisame76.minato.test.obb;

import android.app.Application;
import android.content.Context;
import android.util.Log;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;

/**
 * ADR-0049's test 8: an app reads its OBB, which the user put into its OBB folder (on Android
 * tools/android-reference.sh pushes it there, in Minato the library moves it in at the import). It logs, with the tag
 * obb-test, its OBB folder as getObbDir() gives it and how many getObbDirs() gives, the folder's files, its main OBB's
 * size and lines read through that folder and through /sdcard/Android/obb/<package>/, and that it has no patch OBB,
 * then "done"; tools/android-reference.sh compares it with Android 15's log.
 */
public class ObbApplication extends Application {
    private static final String TAG = "obb-test";
    private static final String PACKAGE = "io.github.kisame76.minato.test.obb";
    private static final String MAIN = "main.1." + PACKAGE + ".obb";
    private static final String PATCH = "patch.1." + PACKAGE + ".obb";

    @Override
    protected void attachBaseContext(Context base) {
        super.attachBaseContext(base);
        try {
            report(base);
        } catch (Throwable t) {
            Log.i(TAG, "failed: " + t);
            // Its stack, which differs by device, is only shown.
            Log.i(TAG + "-device", Log.getStackTraceString(t));
        }
        Log.i(TAG, "done");
    }

    static void report(Context base) throws IOException {
        File folder = base.getObbDir();
        Log.i(TAG, "obbDir " + folder);
        Log.i(TAG, "obbDirs " + base.getObbDirs().length);
        String[] names = folder.list();
        if (names != null) Arrays.sort(names);
        Log.i(TAG, "files " + (names == null ? "none" : String.join(" ", names)));
        read("main", new File(folder, MAIN));
        read("main by path", new File("/sdcard/Android/obb/" + PACKAGE, MAIN));
        Log.i(TAG, "patch exists " + new File(folder, PATCH).exists());
    }

    /** Logs `file`'s size and each of its lines. */
    static void read(String what, File file) throws IOException {
        Log.i(TAG, what + " size " + file.length() + " canRead " + file.canRead());
        try (InputStream in = new FileInputStream(file)) {
            for (String line : new String(in.readAllBytes(), StandardCharsets.UTF_8).split("\n")) {
                Log.i(TAG, what + " line " + line);
            }
        }
    }
}
