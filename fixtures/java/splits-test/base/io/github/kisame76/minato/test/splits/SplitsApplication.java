package io.github.kisame76.minato.test.splits;

import android.app.Application;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.util.Log;
import java.io.File;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * What a split set gives an app at run time (ADR-0048 c), the guest side): the splits as ApplicationInfo and
 * PackageInfo list them, a class that only the feature split holds, the feature split's asset, a string that only the
 * language split holds, and the JNI library of the arm64 split. Each part logs with the tag splits-test; a part that
 * throws logs "failed" and the next one runs, so that every run reaches "done". Paths are logged by file name, as the
 * code folder differs by device.
 */
public class SplitsApplication extends Application {
    static final String TAG = "splits-test";

    static native String nativeAnswer();

    static native String nativeAssets(android.content.res.AssetManager assets);

    private interface Part {
        void run() throws Exception;
    }

    private static List<String> names(String[] paths) {
        List<String> names = new ArrayList<>();
        if (paths != null) {
            for (String path : paths) {
                names.add(new File(path).getName());
            }
        }
        return names;
    }

    private void info() throws Exception {
        ApplicationInfo ai = getApplicationInfo();
        Log.i(TAG, "source: " + new File(ai.sourceDir).getName() + ", code path: " + getPackageCodePath().endsWith(
                "/base.apk"));
        Log.i(TAG, "split names: " + Arrays.toString(ai.splitNames));
        Log.i(TAG, "split source dirs: " + names(ai.splitSourceDirs));
        Log.i(TAG, "split public source dirs: " + names(ai.splitPublicSourceDirs));
        PackageInfo pi = getPackageManager().getPackageInfo(getPackageName(), 0);
        Log.i(TAG, "package info splits: " + Arrays.toString(pi.splitNames) + ", application "
                + Arrays.toString(pi.applicationInfo.splitNames));
        ApplicationInfo viaManager = getPackageManager().getApplicationInfo(getPackageName(), 0);
        Log.i(TAG, "manager info splits: " + names(viaManager.splitSourceDirs));
    }

    private void feature() throws Exception {
        Class<?> type = Class.forName("io.github.kisame76.minato.test.splits.feature.FeatureClass");
        Log.i(TAG, "feature class: " + type.getMethod("answer").invoke(null));
        Log.i(TAG, "same loader as the app: " + (type.getClassLoader() == getClassLoader()));
        for (String name : new String[] {"feature.txt", "base.txt"}) {
            try (InputStream in = getAssets().open(name)) {
                byte[] bytes = new byte[100];
                Log.i(TAG, name + ": " + new String(bytes, 0, in.read(bytes)).trim());
            }
        }
        Log.i(TAG, "assets: " + Arrays.toString(getAssets().list("")));
    }

    private void language() {
        Log.i(TAG, "greeting: " + getString(R.string.greeting));
    }

    private void library() {
        System.loadLibrary("splits_test");
        Log.i(TAG, "library: " + nativeAnswer());
        Log.i(TAG, "native assets: " + nativeAssets(getAssets()));
    }

    @Override
    public void onCreate() {
        super.onCreate();
        for (Part part : Arrays.<Part>asList(this::info, this::feature, this::language, this::library)) {
            try {
                part.run();
            } catch (Throwable t) {
                Log.i(TAG, "failed: " + t.getClass().getName() + ": " + t.getMessage());
            }
        }
        Log.i(TAG, "done");
    }
}
