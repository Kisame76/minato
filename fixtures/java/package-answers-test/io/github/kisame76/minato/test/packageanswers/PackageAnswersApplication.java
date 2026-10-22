package io.github.kisame76.minato.test.packageanswers;

import android.app.Application;
import android.content.ComponentName;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.ChangedPackages;
import android.content.pm.InstrumentationInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.PermissionGroupInfo;
import android.content.pm.PermissionInfo;
import android.content.pm.ProviderInfo;
import android.content.pm.ResolveInfo;
import android.content.pm.SharedLibraryInfo;
import android.content.pm.VersionedPackage;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.graphics.Rect;
import android.graphics.drawable.Drawable;
import android.os.Process;
import android.os.UserHandle;
import android.util.Log;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import java.util.Locale;

/**
 * The test of the package manager's answers for the app's own package, without a window (the package manager stub
 * plan). It logs under the tag package-answers-test, part by part:
 * <ul>
 *   <li>P1: the Resources of getResourcesForApplication by name, by ApplicationInfo with and without a configuration,
 *       and of getResourcesForActivity: a string of the app, its locale, and whether they are the context's Resources
 *       or share its AssetManager; and what an unknown package or activity meets.</li>
 *   <li>P2: the app's identity: its launch intents (an INFO activity first), the packages of its uid and the uid's
 *       name, its uid and gids (INTERNET's inet), its PackageInfo by version, its enabled setting, the "android"
 *       package, and whether the installed packages and applications it sees hold itself and "android". The names it
 *       sees go to the tag package-answers-test-device, which the test does not compare: they depend on the device.
 *       getCarLaunchIntentForPackage and getNamesForUids are hidden API, which Android 15 does not let an app
 *       call.</li>
 *   <li>P3: the drawables: the icon, logo and banner of the application and of an activity by name and by Intent
 *       (the activity's own icon, the application's for an activity without), getDrawable, the default icon, and
 *       the badge calls for the one user (no badge). A drawable is logged as its class and its size in dp, not its
 *       pixels.</li>
 * </ul>
 * Public API only. tools/android-reference.sh takes the log from Android 15, and tools/package-answers-check.sh
 * compares Minato's with it.
 */
public class PackageAnswersApplication extends Application {
    private static final String TAG = "package-answers-test";
    /** The device's own answers, which tools/android-reference.sh keeps apart from the comparison. */
    private static final String PROBE = "package-answers-test-device";
    private static final String ABSENT = "io.github.kisame76.minato.test.absent";
    /** An application uid no package has: the last one. */
    private static final int NO_UID = Process.LAST_APPLICATION_UID;

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

    /** The icon of an activity whose icon is 0 and whose application's is not: the application's. */
    private Drawable missingIcon(PackageManager pm, ComponentName activity) throws Exception {
        ActivityInfo info = pm.getActivityInfo(activity, 0);
        info.icon = 0;
        return info.loadIcon(pm);
    }

    /** The activities queryIntentActivityOptions gives, in order, and whether every filter was dropped. */
    private static String options(PackageManager pm, ComponentName caller, Intent[] specifics, Intent intent, int flags) {
        List<String> names = new ArrayList<>();
        boolean noFilters = true;
        for (ResolveInfo info : pm.queryIntentActivityOptions(caller, specifics, intent, flags)) {
            names.add(info.activityInfo.name.substring(info.activityInfo.name.lastIndexOf('.') + 1));
            noFilters &= info.filter == null;
        }
        return names + (noFilters ? "" : " filtered");
    }

    /** The authorities among the providers that the package declares. */
    private static List<String> own(List<ProviderInfo> providers, String pkg) {
        List<String> authorities = new ArrayList<>();
        for (ProviderInfo info : providers) {
            if (info.packageName.equals(pkg)) {
                authorities.add(info.authority + " in " + info.processName);
            }
        }
        return authorities;
    }

    /** A drawable as its class and its intrinsic size in dp. */
    private String shape(Drawable d) {
        if (d == null) {
            return "null";
        }
        float density = getResources().getDisplayMetrics().density;
        return d.getClass().getSimpleName() + " " + Math.round(d.getIntrinsicWidth() / density) + "x"
                + Math.round(d.getIntrinsicHeight() / density);
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
        // P2: the app's identity.
        probe("launch", () -> log("launch: " + pm.getLaunchIntentForPackage(pkg)));
        probe("launch, unknown", () -> log("launch, unknown: " + pm.getLaunchIntentForPackage(ABSENT)));
        probe("leanback", () -> log("leanback: " + pm.getLeanbackLaunchIntentForPackage(pkg)));
        int uid = Process.myUid();
        probe("uid's packages", () -> log("uid's packages: " + Arrays.toString(pm.getPackagesForUid(uid))
                + ", none's " + Arrays.toString(pm.getPackagesForUid(NO_UID))));
        probe("uid's name", () -> log("uid's name: " + pm.getNameForUid(uid) + ", none's " + pm.getNameForUid(NO_UID)));
        probe("package uid", () -> log("package uid: mine " + (pm.getPackageUid(pkg, 0) == uid)
                + ", application info's " + (getApplicationInfo().uid == uid)));
        probe("package uid, unknown", () -> log("package uid, unknown: " + pm.getPackageUid(ABSENT, 0)));
        probe("gids", () -> log("gids: " + Arrays.toString(pm.getPackageGids(pkg))));
        probe("gids, unknown", () -> log("gids, unknown: " + Arrays.toString(pm.getPackageGids(ABSENT))));
        probe("versioned", () -> {
            PackageInfo info = pm.getPackageInfo(new VersionedPackage(pkg, 1), 0);
            log("versioned: " + info.packageName + " " + info.getLongVersionCode());
            info = pm.getPackageInfo(new VersionedPackage(pkg, PackageManager.VERSION_CODE_HIGHEST), 0);
            log("versioned, highest: " + info.packageName + " " + info.getLongVersionCode());
        });
        probe("versioned, other version", () -> log("versioned, other version: "
                + pm.getPackageInfo(new VersionedPackage(pkg, 2), 0).packageName));
        probe("versioned, unknown", () -> log("versioned, unknown: "
                + pm.getPackageInfo(new VersionedPackage(ABSENT, 1), 0).packageName));
        probe("enabled setting", () -> log("enabled setting: " + pm.getApplicationEnabledSetting(pkg)));
        probe("enabled setting, unknown", () -> log("enabled setting, unknown: "
                + pm.getApplicationEnabledSetting(ABSENT)));
        probe("android", () -> {
            ApplicationInfo info = pm.getApplicationInfo("android", 0);
            log("android: " + info.packageName + ", uid " + info.uid + ", system "
                    + ((info.flags & ApplicationInfo.FLAG_SYSTEM) != 0) + ", source " + info.sourceDir);
        });
        probe("android's package", () -> {
            PackageInfo info = pm.getPackageInfo("android", 0);
            log("android's package: " + info.packageName + ", shared user " + info.sharedUserId);
        });
        probe("android's resources", () -> {
            Resources r = pm.getResourcesForApplication("android");
            log("android's resources: ok " + r.getString(android.R.string.ok) + ", system's "
                    + (r == Resources.getSystem()) + ", system's assets " + (r.getAssets() == Resources.getSystem().getAssets()));
        });
        probe("installed packages", () -> {
            List<String> names = new ArrayList<>();
            for (PackageInfo info : pm.getInstalledPackages(0)) {
                names.add(info.packageName);
            }
            log("installed packages: mine " + names.contains(pkg) + ", android " + names.contains("android"));
            Collections.sort(names);
            Log.i(PROBE, "installed packages " + names.size() + ": " + names);
        });
        probe("installed applications", () -> {
            List<String> names = new ArrayList<>();
            for (ApplicationInfo info : pm.getInstalledApplications(0)) {
                names.add(info.packageName);
            }
            log("installed applications: mine " + names.contains(pkg) + ", android " + names.contains("android"));
            Collections.sort(names);
            Log.i(PROBE, "installed applications " + names.size() + ": " + names);
        });
        // P3: drawables.
        ComponentName main = new ComponentName(this, "android.app.Activity");
        ComponentName list = new ComponentName(this, "android.app.ListActivity");
        Intent mainIntent = new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER).setPackage(pkg);
        Intent unresolved = new Intent("io.github.kisame76.minato.test.NOBODY").setPackage(pkg);
        probe("application", () -> log("application: icon " + shape(pm.getApplicationIcon(pkg)) + ", logo "
                + shape(pm.getApplicationLogo(pkg)) + ", banner " + shape(pm.getApplicationBanner(pkg))));
        probe("application by info", () -> log("application by info: icon " + shape(pm.getApplicationIcon(getApplicationInfo()))
                + ", logo " + shape(pm.getApplicationLogo(getApplicationInfo())) + ", banner "
                + shape(pm.getApplicationBanner(getApplicationInfo()))));
        probe("application, unknown", () -> log("application, unknown: " + shape(pm.getApplicationIcon(ABSENT))));
        probe("application logo, unknown", () -> log("application logo, unknown: " + shape(pm.getApplicationLogo(ABSENT))));
        probe("application banner, unknown",
                () -> log("application banner, unknown: " + shape(pm.getApplicationBanner(ABSENT))));
        probe("activity", () -> log("activity: icon " + shape(pm.getActivityIcon(main)) + ", logo "
                + shape(pm.getActivityLogo(main)) + ", banner " + shape(pm.getActivityBanner(main))));
        probe("activity with an icon", () -> log("activity with an icon: icon " + shape(pm.getActivityIcon(list))
                + ", logo " + shape(pm.getActivityLogo(list)) + ", banner " + shape(pm.getActivityBanner(list))));
        probe("activity with a broken icon", () -> log("activity with a broken icon: "
                + shape(pm.getActivityIcon(new ComponentName(this, "android.app.ExpandableListActivity")))));
        probe("icon that fails to load", () -> {
            ActivityInfo broken = pm.getActivityInfo(main, 0);
            broken.icon = getResources().getIdentifier("greeting", "string", pkg);
            log("icon that fails to load: " + shape(broken.loadIcon(pm)) + ", icon 0 " + shape(missingIcon(pm, main)));
        });
        probe("activity by intent", () -> log("activity by intent: icon " + shape(pm.getActivityIcon(mainIntent))
                + ", logo " + shape(pm.getActivityLogo(mainIntent)) + ", banner " + shape(pm.getActivityBanner(mainIntent))));
        probe("activity by component intent", () -> log("activity by component intent: "
                + shape(pm.getActivityIcon(new Intent().setComponent(list)))));
        probe("activity, unresolved", () -> log("activity, unresolved: " + shape(pm.getActivityIcon(unresolved))));
        probe("activity logo, unresolved", () -> log("activity logo, unresolved: " + shape(pm.getActivityLogo(unresolved))));
        probe("activity banner, unresolved",
                () -> log("activity banner, unresolved: " + shape(pm.getActivityBanner(unresolved))));
        probe("activity, unknown", () -> log("activity, unknown: "
                + shape(pm.getActivityIcon(new ComponentName(this, pkg + ".Missing")))));
        probe("default icon", () -> log("default icon: " + shape(pm.getDefaultActivityIcon())));
        int icon = getApplicationInfo().icon;
        probe("drawable", () -> log("drawable: " + shape(pm.getDrawable(pkg, icon, null)) + ", with info "
                + shape(pm.getDrawable(pkg, icon, getApplicationInfo())) + ", no resource " + shape(pm.getDrawable(pkg, 0, null))
                + ", unknown package " + shape(pm.getDrawable(ABSENT, icon, null))));
        probe("drawable, system", () -> log("drawable, system: " + shape(pm.getDrawable("android", android.R.drawable.ic_menu_add, null))));
        probe("loaded icon", () -> log("loaded icon: application " + shape(getApplicationInfo().loadIcon(pm)) + ", logo "
                + shape(getApplicationInfo().loadLogo(pm)) + ", activity " + shape(pm.getActivityInfo(list, 0).loadIcon(pm))
                + ", activity logo " + shape(pm.getActivityInfo(main, 0).loadLogo(pm))));
        UserHandle me = Process.myUserHandle();
        probe("badges", () -> {
            Drawable d = pm.getApplicationIcon(pkg);
            log("badges: icon same " + (pm.getUserBadgedIcon(d, me) == d) + ", density same "
                    + (pm.getUserBadgedDrawableForDensity(d, me, new Rect(0, 0, 10, 10), 0) == d) + ", density, no rect "
                    + (pm.getUserBadgedDrawableForDensity(d, me, null, 0) == d)
                    + ", badge " + shape(pm.getUserBadgeForDensity(me, 0))
                    + ", badge, no background " + shape(pm.getUserBadgeForDensityNoBackground(me, 0))
                    + ", label " + pm.getUserBadgedLabel("label", me));
        });
        // P4: lists.
        String group = pkg + ".GROUP";
        probe("permission groups", () -> {
            List<String> names = new ArrayList<>();
            for (PermissionGroupInfo info : pm.getAllPermissionGroups(0)) {
                names.add(info.name);
            }
            log("permission groups: own " + names.contains(group) + ", bare " + names.contains(pkg + ".BARE")
                    + ", camera "
                    + names.contains("android.permission-group.CAMERA") + ", location "
                    + names.contains("android.permission-group.LOCATION"));
            Log.i(PROBE, "permission groups " + names.size() + ": " + names);
        });
        probe("permission group", () -> {
            PermissionGroupInfo info = pm.getPermissionGroupInfo(group, 0);
            log("permission group: " + info.name + ", priority " + info.priority + ", package " + info.packageName
                    + ", label " + info.loadLabel(pm));
        });
        probe("permission group, bare", () -> {
            PermissionGroupInfo info = pm.getPermissionGroupInfo(pkg + ".BARE", 0);
            log("permission group, bare: " + info.name + ", priority " + info.priority + ", description "
                    + info.descriptionRes + ", flags " + info.flags + ", label " + info.loadLabel(pm)
                    + ", icon " + info.icon);
        });
        probe("permissions of an undeclared group", () -> log("permissions of an undeclared group: "
                + pm.queryPermissionsByGroup(pkg + ".UNDECLARED", 0)));
        probe("permission groups, duplicates", () -> {
            int twice = 0;
            for (PermissionGroupInfo info : pm.getAllPermissionGroups(0)) {
                twice += info.name.equals(pkg + ".TWICE") ? 1 : 0;
            }
            PermissionGroupInfo second = pm.getPermissionGroupInfo(pkg + ".TWICE", 0);
            log("permission groups, duplicates: twice " + twice + " (label " + second.loadLabel(pm) + ")");
        });
        probe("permission group, unknown", () -> log("permission group, unknown: " + pm.getPermissionGroupInfo(ABSENT, 0)));
        probe("permissions by group", () -> {
            List<String> names = new ArrayList<>();
            for (PermissionInfo info : pm.queryPermissionsByGroup(group, 0)) {
                names.add(info.name);
            }
            log("permissions by group: " + names);
        });
        probe("permissions of the camera group", () -> {
            List<String> names = new ArrayList<>();
            for (PermissionInfo info : pm.queryPermissionsByGroup("android.permission-group.CAMERA", 0)) {
                names.add(info.name);
            }
            log("permissions of the camera group: " + names);
        });
        probe("permissions without a group", () -> {
            List<String> names = new ArrayList<>();
            for (PermissionInfo info : pm.queryPermissionsByGroup(null, 0)) {
                names.add(info.name);
            }
            log("permissions without a group: ungrouped " + names.contains(pkg + ".UNGROUPED") + ", grouped "
                    + names.contains(pkg + ".GROUPED") + ", internet " + names.contains("android.permission.INTERNET"));
        });
        probe("permissions, unknown group", () -> log("permissions, unknown group: " + pm.queryPermissionsByGroup(ABSENT, 0)));
        probe("instrumentation", () -> {
            List<String> names = new ArrayList<>();
            for (InstrumentationInfo info : pm.queryInstrumentation(pkg, 0)) {
                names.add(info.name + " for " + info.targetPackage + " label " + info.loadLabel(pm));
            }
            List<String> others = new ArrayList<>();
            for (InstrumentationInfo info : pm.queryInstrumentation(ABSENT, 0)) {
                others.add(info.name + " in " + info.targetProcesses + " profiling " + info.handleProfiling
                        + " functional " + info.functionalTest + " source " + (info.sourceDir != null)
                        + " data " + (info.dataDir != null));
            }
            log("instrumentation: " + names + ", for another package " + others
                    + ", all " + (pm.queryInstrumentation(null, 0).size() >= 2));
        });
        probe("instrumentation info", () -> log("instrumentation info: "
                + pm.getInstrumentationInfo(new ComponentName(pkg, "android.test.InstrumentationTestRunner"), 0).name));
        probe("instrumentation info, unknown", () -> log("instrumentation info, unknown: "
                + pm.getInstrumentationInfo(new ComponentName(pkg, "Missing"), 0)));
        probe("providers", () -> {
            List<ProviderInfo> all = pm.queryContentProviders(null, uid, 0);
            log("providers: all " + own(all, pkg) + ", of the process " + own(pm.queryContentProviders(pkg + ":other", uid, 0), pkg)
                    + ", of another process " + own(pm.queryContentProviders(pkg, uid, 0), pkg)
                    + ", of another uid " + own(pm.queryContentProviders(pkg + ":other", NO_UID, 0), pkg)
                    + ", of any uid " + own(pm.queryContentProviders(null, NO_UID, 0), pkg));
            Log.i(PROBE, "providers " + all.size());
        });
        probe("activity options", () -> {
            Intent intent = new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER).setPackage(pkg);
            List<String> names = new ArrayList<>();
            for (ResolveInfo info : pm.queryIntentActivityOptions(null, null, intent, 0)) {
                names.add(info.activityInfo.name);
            }
            log("activity options: " + names);
        });
        probe("activity options, specifics", () -> {
            Intent intent = new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER).setPackage(pkg);
            Intent[] specifics = {new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_INFO).setPackage(pkg),
                    new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LEANBACK_LAUNCHER).setPackage(pkg)};
            List<String> names = new ArrayList<>();
            for (ResolveInfo info : pm.queryIntentActivityOptions(null, specifics, intent, 0)) {
                names.add(info.activityInfo.name);
            }
            List<String> withCaller = new ArrayList<>();
            for (ResolveInfo info : pm.queryIntentActivityOptions(new ComponentName(pkg, "android.app.ListActivity"),
                    specifics, intent, 0)) {
                withCaller.add(info.activityInfo.name);
            }
            log("activity options, specifics: " + names + ", with a caller " + withCaller);
        });
        // The three activities share the action G; G1 and G3 have S1, G2 has S2.
        Intent generic = new Intent(pkg + ".G");
        Intent s1 = new Intent(pkg + ".S1");
        Intent s2 = new Intent(pkg + ".S2");
        ComponentName g1 = new ComponentName(pkg, "android.app.AliasActivity");
        ComponentName g3 = new ComponentName(pkg, "android.app.ActivityGroup");
        probe("activity options, generic", () -> log("activity options, generic: "
                + options(pm, null, null, generic, 0) + ", with filters "
                + options(pm, null, null, generic, PackageManager.GET_RESOLVED_FILTER)));
        probe("activity options, one action", () -> log("activity options, one action: "
                + options(pm, null, new Intent[] {s2}, generic, 0)));
        probe("activity options, two actions", () -> log("activity options, two actions: "
                + options(pm, null, new Intent[] {s2, s1}, generic, 0)));
        probe("activity options, the same action", () -> log("activity options, the same action: "
                + options(pm, null, new Intent[] {s1}, generic, 0)));
        probe("activity options, a component", () -> log("activity options, a component: "
                + options(pm, null, new Intent[] {new Intent().setComponent(g3)}, generic, 0)));
        probe("activity options, the intent's action", () -> log("activity options, the intent's action: "
                + options(pm, null, new Intent[] {new Intent(pkg + ".G")}, generic, 0)));
        probe("activity options, nobody", () -> log("activity options, nobody: "
                + options(pm, null, new Intent[] {new Intent(pkg + ".NOBODY")}, generic, 0)));
        probe("activity options, a caller", () -> log("activity options, a caller: "
                + options(pm, g1, new Intent[] {s2}, generic, 0) + ", twice "
                + options(pm, g1, new Intent[] {s1, s1}, generic, 0)));
        probe("holding permissions", () -> {
            List<String> names = new ArrayList<>();
            for (PackageInfo info : pm.getPackagesHoldingPermissions(new String[] {"android.permission.INTERNET"}, 0)) {
                names.add(info.packageName);
            }
            log("holding permissions: mine " + names.contains(pkg));
            Log.i(PROBE, "holding permissions " + names.size() + ": " + names);
        });
        probe("shared libraries", () -> {
            List<SharedLibraryInfo> libraries = pm.getSharedLibraries(0);
            String[] names = pm.getSystemSharedLibraryNames();
            log("shared libraries: list " + (libraries != null) + ", names " + (names != null));
            Log.i(PROBE, "shared libraries " + libraries.size() + ", names " + Arrays.toString(names));
        });
        probe("preferred", () -> {
            List<IntentFilter> filters = new ArrayList<>();
            List<ComponentName> components = new ArrayList<>();
            int count = pm.getPreferredActivities(filters, components, pkg);
            log("preferred: activities " + count + " " + filters.size() + " " + components.size()
                    + ", packages " + pm.getPreferredPackages(0));
        });
        probe("changed packages", () -> {
            ChangedPackages changed = pm.getChangedPackages(0);
            log("changed packages: " + (changed == null ? "null" : "list " + (changed.getPackageNames() != null)));
        });
        log("done");
    }
}
