package io.github.kisame76.minato.test.packageanswers;

import android.app.Application;
import android.content.ComponentName;
import android.content.pm.PackageManager;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.util.Log;
import java.util.Locale;

/**
 * The test of the package manager's answers for the app's own package, without a window (the package manager stub
 * plan). It logs under the tag package-answers-test, part by part:
 * <ul>
 *   <li>P1: the Resources of getResourcesForApplication by name, by ApplicationInfo with and without a configuration,
 *       and of getResourcesForActivity: a string of the app, its locale, and whether they are the context's Resources
 *       or share its AssetManager; and what an unknown package or activity meets.</li>
 * </ul>
 * Public API only. tools/android-reference.sh takes the log from Android 15, and tools/package-answers-check.sh
 * compares Minato's with it.
 */
public class PackageAnswersApplication extends Application {
    private static final String TAG = "package-answers-test";

    private interface Body {
        void run() throws Exception;
    }

    private static void log(String line) {
        Log.i(TAG, line);
    }

    private static void probe(String name, Body body) {
        try {
            body.run();
        } catch (Throwable t) {
            log(name + ": " + t.getClass().getName() + ": " + t.getMessage());
        }
    }

    /** What a Resources holds, measured against the context's own. */
    private void describe(String name, Resources r) {
        Resources own = getResources();
        int greeting = r.getIdentifier("greeting", "string", getPackageName());
        log(name + ": greeting " + r.getString(greeting) + ", locale " + r.getConfiguration().getLocales().get(0)
                + ", same locale " + r.getConfiguration().getLocales().equals(own.getConfiguration().getLocales())
                + ", same density " + (r.getDisplayMetrics().densityDpi == own.getDisplayMetrics().densityDpi)
                + ", same orientation " + (r.getConfiguration().orientation == own.getConfiguration().orientation)
                + ", context's " + (r == own) + ", context's assets " + (r.getAssets() == own.getAssets())
                + ", system ok " + r.getString(android.R.string.ok));
    }

    @Override
    public void onCreate() {
        super.onCreate();
        PackageManager pm = getPackageManager();
        String pkg = getPackageName();
        Configuration german = new Configuration();
        german.setLocale(Locale.GERMAN);

        // P1: Resources.
        probe("by name", () -> describe("by name", pm.getResourcesForApplication(pkg)));
        probe("by info", () -> describe("by info", pm.getResourcesForApplication(getApplicationInfo())));
        probe("by info, no configuration",
                () -> describe("by info, no configuration", pm.getResourcesForApplication(getApplicationInfo(), null)));
        probe("by info, German",
                () -> describe("by info, German", pm.getResourcesForApplication(getApplicationInfo(), german)));
        probe("by activity", () -> describe("by activity",
                pm.getResourcesForActivity(new ComponentName(this, "android.app.Activity"))));
        probe("twice", () -> {
            Resources first = pm.getResourcesForApplication(pkg);
            Resources second = pm.getResourcesForApplication(pkg);
            log("twice: same " + (first == second) + ", same assets " + (first.getAssets() == second.getAssets()));
        });
        probe("unknown package", () -> describe("unknown package",
                pm.getResourcesForApplication("io.github.kisame76.minato.test.absent")));
        probe("unknown activity", () -> describe("unknown activity",
                pm.getResourcesForActivity(new ComponentName(this, pkg + ".Missing"))));
        probe("other package's activity", () -> describe("other package's activity",
                pm.getResourcesForActivity(new ComponentName("io.github.kisame76.minato.test.absent", "Main"))));
        log("done");
    }
}
