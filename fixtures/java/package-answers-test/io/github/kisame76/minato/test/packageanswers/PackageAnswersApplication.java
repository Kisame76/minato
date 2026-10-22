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
import android.content.pm.SigningInfo;
import android.graphics.Rect;
import android.graphics.drawable.Drawable;
import android.os.Process;
import android.os.UserHandle;
import android.util.Log;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;
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

    /**
     * A ZIP whose AndroidManifest.xml is binary XML written here: a root element with one attribute (no namespace, a
     * string value) and one child element without attributes. No resource map, no namespace: enough for a parser to
     * walk, and for the package parser to find what the manifest lacks.
     */
    private static File oddArchive(File dir, String name, String root, String attribute, String value, String child)
            throws IOException {
        String[] strings = {attribute, value, root, child};
        java.io.ByteArrayOutputStream pool = new java.io.ByteArrayOutputStream();
        int[] offsets = new int[strings.length];
        for (int i = 0; i < strings.length; i++) {
            offsets[i] = pool.size();
            byte[] chars = strings[i].getBytes(java.nio.charset.StandardCharsets.UTF_16LE);
            pool.write(strings[i].length() & 0xff);
            pool.write(strings[i].length() >> 8);
            pool.write(chars);
            pool.write(0);
            pool.write(0);
        }
        while (pool.size() % 4 != 0) {
            pool.write(0);
        }
        int poolSize = 28 + 4 * strings.length + pool.size();
        java.nio.ByteBuffer b = java.nio.ByteBuffer.allocate(8 + poolSize + 56 + 36 + 24 + 24)
                .order(java.nio.ByteOrder.LITTLE_ENDIAN);
        b.putShort((short) 0x0003).putShort((short) 8).putInt(b.capacity());
        b.putShort((short) 0x0001).putShort((short) 28).putInt(poolSize).putInt(strings.length).putInt(0).putInt(0)
                .putInt(28 + 4 * strings.length).putInt(0);
        for (int offset : offsets) {
            b.putInt(offset);
        }
        b.put(pool.toByteArray());
        // <root attribute="value">: name index 2, the attribute's name 0 and string 1.
        b.putShort((short) 0x0102).putShort((short) 16).putInt(56).putInt(1).putInt(-1);
        b.putInt(-1).putInt(2).putShort((short) 20).putShort((short) 20).putShort((short) 1).putShort((short) 0)
                .putShort((short) 0).putShort((short) 0);
        b.putInt(-1).putInt(0).putInt(1).putShort((short) 8).put((byte) 0).put((byte) 3).putInt(1);
        // <child/>: name index 3.
        b.putShort((short) 0x0102).putShort((short) 16).putInt(36).putInt(2).putInt(-1);
        b.putInt(-1).putInt(3).putShort((short) 20).putShort((short) 20).putShort((short) 0).putShort((short) 0)
                .putShort((short) 0).putShort((short) 0);
        b.putShort((short) 0x0103).putShort((short) 16).putInt(24).putInt(3).putInt(-1).putInt(-1).putInt(3);
        b.putShort((short) 0x0103).putShort((short) 16).putInt(24).putInt(4).putInt(-1).putInt(-1).putInt(2);
        File file = new File(dir, name);
        try (ZipOutputStream out = new ZipOutputStream(new FileOutputStream(file))) {
            out.putNextEntry(new ZipEntry("AndroidManifest.xml"));
            out.write(b.array(), 0, b.position());
            out.closeEntry();
        }
        return file;
    }

    /** Whether the package info marks the permission granted. */
    private static boolean granted(PackageInfo info, String permission) {
        int index = Arrays.asList(info.requestedPermissions).indexOf(permission);
        return (info.requestedPermissionsFlags[index] & PackageInfo.REQUESTED_PERMISSION_GRANTED) != 0;
    }

    private static void copyFile(File from, File to) throws IOException {
        try (InputStream in = new java.io.FileInputStream(from); OutputStream out = new FileOutputStream(to)) {
            byte[] buffer = new byte[65536];
            for (int n; (n = in.read(buffer)) > 0; ) {
                out.write(buffer, 0, n);
            }
        }
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
        // P5: archives.
        String code = getPackageCodePath();
        probe("archive", () -> {
            PackageInfo info = pm.getPackageArchiveInfo(code, 0);
            ApplicationInfo app = info.applicationInfo;
            log("archive: " + info.packageName + " " + info.versionName + " " + info.getLongVersionCode()
                    + ", source " + code.equals(app.sourceDir) + " " + code.equals(app.publicSourceDir)
                    + ", installed " + ((app.flags & ApplicationInfo.FLAG_INSTALLED) != 0)
                    + ", target " + app.targetSdkVersion + ", min " + app.minSdkVersion + ", enabled " + app.enabled
                    + ", components " + (info.activities == null) + " " + (info.permissions == null) + " "
                    + (info.requestedPermissions == null) + " " + (info.signingInfo == null)
                    + ", uid " + app.uid + ", data " + app.dataDir + ", library " + app.nativeLibraryDir
                    + ", label " + app.nonLocalizedLabel + " " + (app.labelRes != 0) + ", icon " + (app.icon != 0));
        });
        probe("archive, components", () -> {
            PackageInfo info = pm.getPackageArchiveInfo(code, PackageManager.GET_ACTIVITIES
                    | PackageManager.GET_PERMISSIONS | PackageManager.GET_PROVIDERS | PackageManager.GET_META_DATA
                    | PackageManager.GET_INSTRUMENTATION | PackageManager.GET_RECEIVERS | PackageManager.GET_SERVICES);
            List<String> requested = Arrays.asList(info.requestedPermissions);
            log("archive, components: activities " + info.activities.length + " " + info.activities[0].name
                    + ", permissions " + info.permissions.length + ", requested internet "
                    + requested.contains("android.permission.INTERNET") + ", instrumentation "
                    + info.instrumentation.length + ", granted " + granted(info, "android.permission.INTERNET")
                    + ", providers " + info.providers.length + ", receivers "
                    + (info.receivers == null) + ", services " + (info.services == null)
                    + ", activity's application " + (info.activities[0].applicationInfo.packageName.equals(pkg)));
        });
        File cache = getCacheDir();
        probe("archive, copy", () -> {
            File copy = new File(cache, "copy.apk");
            copyFile(new File(code), copy);
            PackageInfo info = pm.getPackageArchiveInfo(copy.getPath(), 0);
            log("archive, copy: " + info.packageName + ", source " + copy.getPath().equals(info.applicationInfo.sourceDir));
            copy.delete();
        });
        probe("archive, signing", () -> {
            PackageInfo info = pm.getPackageArchiveInfo(code, PackageManager.GET_SIGNING_CERTIFICATES);
            SigningInfo signing = info == null ? null : info.signingInfo;
            Log.i(PROBE, "archive signing: " + (signing != null ? signing.getApkContentsSigners().length : -1));
        });
        probe("archive, not an archive", () -> {
            File empty = new File(cache, "empty.apk");
            new FileOutputStream(empty).close();
            File garbage = new File(cache, "garbage.apk");
            try (OutputStream out = new FileOutputStream(garbage)) {
                for (int i = 0; i < 4096; i++) {
                    out.write(i * 31);
                }
            }
            File truncated = new File(cache, "truncated.apk");
            try (InputStream in = new java.io.FileInputStream(code); OutputStream out = new FileOutputStream(truncated)) {
                byte[] head = new byte[8192];
                out.write(head, 0, in.read(head));
            }
            File noManifest = new File(cache, "no-manifest.apk");
            try (ZipOutputStream out = new ZipOutputStream(new FileOutputStream(noManifest))) {
                out.putNextEntry(new ZipEntry("classes.dex"));
                out.write(new byte[64]);
                out.closeEntry();
            }
            File badManifest = new File(cache, "bad-manifest.apk");
            try (ZipOutputStream out = new ZipOutputStream(new FileOutputStream(badManifest))) {
                out.putNextEntry(new ZipEntry("AndroidManifest.xml"));
                for (int i = 0; i < 512; i++) {
                    out.write(i * 7);
                }
                out.closeEntry();
            }
            log("archive, not an archive: missing " + pm.getPackageArchiveInfo(cache + "/missing.apk", 0)
                    + ", empty " + pm.getPackageArchiveInfo(empty.getPath(), 0)
                    + ", garbage " + pm.getPackageArchiveInfo(garbage.getPath(), 0)
                    + ", truncated " + pm.getPackageArchiveInfo(truncated.getPath(), 0)
                    + ", folder " + pm.getPackageArchiveInfo(cache.getPath(), 0)
                    + ", no manifest " + pm.getPackageArchiveInfo(noManifest.getPath(), 0)
                    + ", bad manifest " + pm.getPackageArchiveInfo(badManifest.getPath(), 0));
            empty.delete();
            garbage.delete();
            truncated.delete();
            noManifest.delete();
            badManifest.delete();
        });
        probe("archive, oversized", () -> {
            // The real manifest, and a resources.arsc that declares 300 MiB of zeros: no table, so Android parses
            // nothing of it; ours must not inflate it either.
            File big = new File(cache, "big.apk");
            try (java.util.zip.ZipFile zip = new java.util.zip.ZipFile(code);
                    ZipOutputStream out = new ZipOutputStream(new FileOutputStream(big))) {
                ZipEntry manifest = zip.getEntry("AndroidManifest.xml");
                out.putNextEntry(new ZipEntry("AndroidManifest.xml"));
                try (InputStream in = zip.getInputStream(manifest)) {
                    byte[] buffer = new byte[8192];
                    for (int n; (n = in.read(buffer)) > 0; ) {
                        out.write(buffer, 0, n);
                    }
                }
                out.closeEntry();
                out.putNextEntry(new ZipEntry("resources.arsc"));
                byte[] zeros = new byte[1 << 20];
                for (int i = 0; i < 300; i++) {
                    out.write(zeros);
                }
                out.closeEntry();
            }
            log("archive, oversized: " + pm.getPackageArchiveInfo(big.getPath(), 0));
            big.delete();
        });
        probe("archive, odd manifests", () -> {
            // Valid ZIPs with a binary manifest written by hand: aapt2 never makes these.
            File noName = oddArchive(cache, "no-name.apk", "manifest", "package", "x.y", "permission");
            File noPackage = oddArchive(cache, "no-package.apk", "manifest", "versionName", "1", "uses-sdk");
            File wrongRoot = oddArchive(cache, "wrong-root.apk", "wrong", "package", "x.y", "permission");
            File badName = oddArchive(cache, "bad-name.apk", "manifest", "package", "not a package name", "application");
            log("archive, odd manifests: permission without a name " + pm.getPackageArchiveInfo(noName.getPath(), 0)
                    + ", no package " + pm.getPackageArchiveInfo(noPackage.getPath(), 0)
                    + ", wrong root " + pm.getPackageArchiveInfo(wrongRoot.getPath(), 0)
                    + ", bad package name " + (pm.getPackageArchiveInfo(badName.getPath(), 0) == null));
            noName.delete();
            noPackage.delete();
            wrongRoot.delete();
            badName.delete();
        });
        probe("archive, no path", () -> {
            try {
                log("archive, no path: " + pm.getPackageArchiveInfo(null, 0));
            } catch (NullPointerException e) {
                // The message differs: libcore's NullPointerException() is built with ART's description here.
                log("archive, no path: " + e.getClass().getName());
                Log.i(PROBE, "archive, no path: " + e.getMessage());
            }
        });
        log("done");
    }
}
