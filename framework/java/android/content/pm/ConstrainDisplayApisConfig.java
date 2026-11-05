package android.content.pm;

import com.android.minato.services.Unwritten;

/**
 * Placeholder (the Unity test APK retest 11): our class under AOSP's hidden name, which only has to load, since a class apps
 * reflect over names it; every body logs once and throws UnsupportedOperationException. Before, a compile-only stub
 * (ADR-0039 b3.1): ActivityInfo's display API sandboxing names it.
 */
public class ConstrainDisplayApisConfig {
    public ConstrainDisplayApisConfig() {
        throw Unwritten.placeholder("android.content.pm.ConstrainDisplayApisConfig()");
    }

    public boolean getNeverConstrainDisplayApis(ApplicationInfo applicationInfo) {
        throw Unwritten.placeholder(
                "android.content.pm.ConstrainDisplayApisConfig.getNeverConstrainDisplayApis(ApplicationInfo)");
    }

    public boolean getAlwaysConstrainDisplayApis(ApplicationInfo applicationInfo) {
        throw Unwritten.placeholder(
                "android.content.pm.ConstrainDisplayApisConfig.getAlwaysConstrainDisplayApis(ApplicationInfo)");
    }
}
