package io.github.kisame76.minato.test.isolated;

import android.app.Application;
import android.content.ComponentName;
import android.content.Context;
import android.content.pm.ComponentInfo;
import android.content.pm.PackageManager;
import android.util.Log;
import java.io.InputStream;
import java.util.Arrays;

/**
 * What a set whose base asks for isolatedSplits gives an app (ADR-0048 c, named limit): each split has a class loader
 * of its own, so the app's loader does not see a split's classes; a split's Context (createContextForSplit) gives its
 * loader, whose parent is the loader of the split it uses, else the app's, and its Resources and assets. Each part logs
 * with the tag isolated-splits-test; a part that throws logs "failed" and the next one runs, so that every run reaches
 * "done".
 */
public class IsolatedApplication extends Application {
    static final String TAG = "isolated-splits-test";
    static final String FEATURE = "io.github.kisame76.minato.test.isolated.feature.";
    static final String SECOND = "io.github.kisame76.minato.test.isolated.second.";

    private interface Lookup {
        String get() throws Exception;
    }

    private static void log(String what, Lookup lookup) {
        String answer;
        try {
            answer = lookup.get();
        } catch (Throwable t) {
            answer = "failed, " + t.getClass().getName();
        }
        Log.i(TAG, what + ": " + answer);
    }

    private static String describe(ComponentInfo info) {
        return info.name.substring(info.name.lastIndexOf('.') + 1) + ", split " + info.splitName;
    }

    @Override
    public void onCreate() {
        super.onCreate();
        String pkg = getPackageName();
        PackageManager pm = getPackageManager();
        log("split names", () -> Arrays.toString(getApplicationInfo().splitNames));
        // The app's own loader and the feature class.
        log("feature class by the app's loader", () ->
                String.valueOf(Class.forName(FEATURE + "FeatureClass", true, getClassLoader())));
        // The split contexts.
        Context feature;
        try {
            feature = createContextForSplit("feature");
        } catch (Throwable t) {
            Log.i(TAG, "context for feature failed: " + t.getClass().getName());
            feature = null;
        }
        final Context featureContext = feature;
        if (featureContext != null) {
            log("feature class by the feature loader", () ->
                    "" + featureContext.getClassLoader().loadClass(FEATURE + "FeatureClass")
                            .getMethod("answer").invoke(null));
            log("feature loader is the app's", () -> "" + (featureContext.getClassLoader() == getClassLoader()));
            log("feature loader's parent is the app's", () ->
                    "" + (featureContext.getClassLoader().getParent() == getClassLoader()));
            log("feature string", () -> featureContext.getString(
                    featureContext.getResources().getIdentifier("feature_label", "string", pkg)));
            log("feature asset", () -> read(featureContext, "feature.txt"));
            log("base asset by the feature context", () -> read(featureContext, "base.txt"));
            log("base string by the feature context", () -> featureContext.getString(R.string.greeting));
        }
        log("feature string by the app", () -> "" + getResources().getIdentifier("feature_label", "string", pkg));
        log("feature asset by the app", () -> read(IsolatedApplication.this, "feature.txt"));
        log("base asset by the app", () -> read(IsolatedApplication.this, "base.txt"));
        // The second split, which uses the first.
        log("second class by the second loader", () -> {
            Context second = createContextForSplit("second");
            Class<?> type = second.getClassLoader().loadClass(SECOND + "SecondClass");
            return "" + type.getMethod("second").invoke(null);
        });
        log("second loader's parent is the feature loader", () ->
                "" + (createContextForSplit("second").getClassLoader().getParent()
                        == createContextForSplit("feature").getClassLoader()));
        // The components of the feature split.
        log("provider", () -> describe(pm.getProviderInfo(new ComponentName(pkg, FEATURE + "FeatureProvider"), 0)));
        log("service", () -> describe(pm.getServiceInfo(new ComponentName(pkg, FEATURE + "FeatureService"), 0)));
        log("receiver", () -> describe(pm.getReceiverInfo(new ComponentName(pkg, FEATURE + "FeatureReceiver"), 0)));
        Log.i(TAG, "done");
    }

    private static String read(Context context, String name) throws Exception {
        try (InputStream in = context.getAssets().open(name)) {
            byte[] bytes = new byte[100];
            return new String(bytes, 0, in.read(bytes)).trim();
        }
    }
}
