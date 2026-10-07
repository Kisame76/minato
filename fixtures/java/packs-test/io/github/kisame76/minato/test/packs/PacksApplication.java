package io.github.kisame76.minato.test.packs;

import android.app.Application;
import android.content.Context;
import android.content.pm.PackageManager;
import android.os.Bundle;
import android.util.Log;
import java.io.File;
import java.util.Arrays;

/**
 * ADR-0050's test 2: an app whose fast-follow and on-demand pack slices Minato keeps finds the meta-data
 * local_testing_dir, which switches Google's Play libraries to their local-testing path, and the slices in the folder
 * local_testing of its external files folder, read-only. It logs, with the tag packs-test (packs-test-own for its twin,
 * whose manifest names its own value), local_testing_dir as getApplicationInfo with GET_META_DATA and the context's
 * ApplicationInfo give it, whether getApplicationInfo without the flag gives metaData, its external files folder, and
 * the files of local_testing with their sizes and access, then "done". Without packs, tools/android-reference.sh
 * compares it with Android 15's log; with packs, tools/packs-check.sh compares it with Minato's.
 */
public class PacksApplication extends Application {
    @Override
    protected void attachBaseContext(Context base) {
        super.attachBaseContext(base);
        String tag = base.getPackageName().endsWith("own") ? "packs-test-own" : "packs-test";
        try {
            report(base, tag);
        } catch (Throwable t) {
            Log.i(tag, "failed: " + t);
            // Its stack, which differs by device, is only shown.
            Log.i(tag + "-device", Log.getStackTraceString(t));
        }
        Log.i(tag, "done");
    }

    static void report(Context base, String tag) throws PackageManager.NameNotFoundException {
        PackageManager manager = base.getPackageManager();
        String name = base.getPackageName();
        Log.i(tag, "GET_META_DATA: " + value(manager.getApplicationInfo(name, PackageManager.GET_META_DATA).metaData));
        Log.i(tag, "without GET_META_DATA: metaData "
                + (manager.getApplicationInfo(name, 0).metaData == null ? "null" : "set"));
        Log.i(tag, "context: " + value(base.getApplicationInfo().metaData));
        File files = base.getExternalFilesDir(null);
        Log.i(tag, "externalFilesDir " + files);
        File folder = new File(files, "local_testing");
        String[] names = folder.list();
        if (names == null) {
            Log.i(tag, "local_testing absent");
            return;
        }
        Arrays.sort(names);
        Log.i(tag, "local_testing files " + names.length);
        for (String file : names) {
            File slice = new File(folder, file);
            Log.i(tag, "local_testing " + file + " size " + slice.length() + " canRead " + slice.canRead()
                    + " canWrite " + slice.canWrite());
        }
    }

    /** local_testing_dir in `metaData`, or that there is no metaData. */
    static String value(Bundle metaData) {
        return metaData == null ? "metaData null" : "local_testing_dir " + metaData.getString("local_testing_dir");
    }
}
