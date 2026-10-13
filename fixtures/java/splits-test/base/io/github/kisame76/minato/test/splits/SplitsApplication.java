package io.github.kisame76.minato.test.splits;

import android.app.Application;
import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.ComponentInfo;
import android.content.pm.PackageManager;
import android.content.pm.ProviderInfo;
import android.content.pm.ServiceInfo;
import android.content.pm.PackageInfo;
import android.content.pm.ResolveInfo;
import android.util.Log;
import java.io.File;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.TreeSet;

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

    private static final String FEATURE = "io.github.kisame76.minato.test.splits.feature.";

    private static String describe(ComponentInfo info) {
        return info.name.substring(info.name.lastIndexOf('.') + 1) + ", split " + info.splitName
                + ", in this application "
                + info.applicationInfo.packageName.equals("io.github.kisame76.minato.test.splits");
    }

    private static List<String> names(ComponentInfo[] infos) {
        TreeSet<String> names = new TreeSet<>();
        if (infos != null) {
            for (ComponentInfo info : infos) {
                names.add(info.name.substring(info.name.lastIndexOf('.') + 1));
            }
        }
        return new ArrayList<>(names);
    }

    private interface Lookup {
        String get() throws Exception;
    }

    /** One lookup's answer, or the class of its exception, so that a lookup that fails does not hide the others. */
    private static void log(String what, Lookup lookup) {
        String answer;
        try {
            answer = lookup.get();
        } catch (Throwable t) {
            answer = "failed, " + t.getClass().getName();
        }
        Log.i(TAG, what + ": " + answer);
    }

    /** The feature split's manifest components, as PackageManager gives them. */
    private void components() {
        PackageManager pm = getPackageManager();
        String pkg = getPackageName();
        log("activity", () -> describe(pm.getActivityInfo(new ComponentName(pkg, FEATURE + "FeatureActivity"), 0)));
        log("activity label", () -> {
            ActivityInfo info = pm.getActivityInfo(new ComponentName(pkg, FEATURE + "FeatureActivity"), 0);
            return info.loadLabel(pm) + ", label resource " + (info.labelRes != 0);
        });
        log("service", () -> describe(pm.getServiceInfo(new ComponentName(pkg, FEATURE + "FeatureService"), 0)));
        log("receiver", () -> describe(pm.getReceiverInfo(new ComponentName(pkg, FEATURE + "FeatureReceiver"), 0)));
        log("provider", () -> describe(pm.getProviderInfo(new ComponentName(pkg, FEATURE + "FeatureProvider"), 0)));
        log("provider by authority", () -> {
            ProviderInfo info = pm.resolveContentProvider(FEATURE.substring(0, FEATURE.length() - 1), 0);
            return info == null ? "none" : describe(info);
        });
        log("duplicate by name", () -> describe(pm.getServiceInfo(new ComponentName(pkg, pkg + ".DupService"),
                PackageManager.MATCH_DISABLED_COMPONENTS)));
        log("duplicate listed", () -> {
            List<String> listed = new ArrayList<>();
            for (ServiceInfo service : pm.getPackageInfo(pkg,
                    PackageManager.GET_SERVICES | PackageManager.MATCH_DISABLED_COMPONENTS).services) {
                if (service.name.endsWith(".DupService")) {
                    listed.add("split " + service.splitName + ", enabled " + service.enabled);
                }
            }
            return listed.toString();
        });
        log("duplicate by action", () -> {
            List<String> found = new ArrayList<>();
            Intent query = new Intent(pkg + ".DUP_ACTION").setPackage(pkg);
            for (ResolveInfo info : pm.queryIntentServices(query, 0)) {
                found.add(info.serviceInfo.name.substring(pkg.length() + 1) + " in split "
                        + info.serviceInfo.splitName);
            }
            return found.size() + " " + found;
        });
        log("package info", () -> {
            PackageInfo pi = pm.getPackageInfo(pkg, PackageManager.GET_ACTIVITIES | PackageManager.GET_SERVICES
                    | PackageManager.GET_RECEIVERS | PackageManager.GET_PROVIDERS);
            return "activities " + names(pi.activities) + ", services " + names(pi.services) + ", receivers "
                    + names(pi.receivers) + ", providers " + names(pi.providers);
        });
        log("meta-data", () -> {
            ApplicationInfo withMeta = pm.getApplicationInfo(pkg, PackageManager.GET_META_DATA);
            return withMeta.metaData == null ? "none" : withMeta.metaData.getString("feature.key");
        });
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
        Log.i(TAG, "application created");
        List<Part> parts = Arrays.<Part>asList(this::info, this::feature, this::language, this::library,
                this::components);
        for (Part part : parts) {
            try {
                part.run();
            } catch (Throwable t) {
                Log.i(TAG, "failed: " + t.getClass().getName() + ": " + t.getMessage());
            }
        }
        Log.i(TAG, "done");
    }
}
