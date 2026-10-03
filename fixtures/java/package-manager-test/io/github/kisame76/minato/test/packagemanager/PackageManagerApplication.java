package io.github.kisame76.minato.test.packagemanager;

import android.app.ActivityThread;
import android.app.Application;
import android.app.ApplicationPackageManager;
import android.content.ComponentName;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.ComponentInfo;
import android.content.pm.ConfigurationInfo;
import android.content.pm.FeatureInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageItemInfo;
import android.content.pm.PackageManager;
import android.content.pm.PermissionInfo;
import android.content.pm.ProviderInfo;
import android.content.pm.ServiceInfo;
import android.os.Bundle;
import android.os.Process;
import android.util.Log;
import java.lang.reflect.Array;
import java.lang.reflect.Field;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.TreeSet;
import java.util.UUID;

/**
 * ADR-0039 b3.5a's test: what PackageManager answers about the app's own package, from its manifest, logged with the
 * tag package-manager-test and compared with Android 15's log (tools/android-reference.sh --app
 * package-manager-test). Each side reports once. Here the static initializer reports through our
 * ApplicationPackageManager, whose constructor takes the ActivityThread, since step a's driver creates no Context. On
 * Android that constructor does not exist (NoSuchMethodError), and onCreate reports through getPackageManager(); ours
 * does not call onCreate before step c.
 *
 * Every public field of the SDK's info classes is logged, by name, since reflection on Android hides the hidden ones
 * and ours does not. What differs by decision is masked, after checking its shape: the code paths' random folders
 * (decision 6), the uid (decision 3), the install times and the application's FLAG_STOPPED. Signatures, the permissions' grants and gids come with
 * b3.5b.
 */
public class PackageManagerApplication extends Application {
    private static final String TAG = "package-manager-test";
    private static final String PACKAGE = "io.github.kisame76.minato.test.packagemanager";
    private static final int FLAGS = PackageManager.GET_ACTIVITIES | PackageManager.GET_RECEIVERS
            | PackageManager.GET_SERVICES | PackageManager.GET_PROVIDERS | PackageManager.GET_META_DATA
            | PackageManager.GET_PERMISSIONS | PackageManager.GET_CONFIGURATIONS;

    private static final String[] ITEM = {"name", "packageName", "labelRes", "nonLocalizedLabel", "icon", "logo",
            "banner", "isArchived", "metaData"};
    private static final String[] COMPONENT = {"applicationInfo", "processName", "splitName", "attributionTags",
            "descriptionRes", "enabled", "exported", "directBootAware"};
    private static final String[] APPLICATION = {"taskAffinity", "permission", "processName", "className",
            "descriptionRes", "theme", "manageSpaceActivityName", "backupAgentName", "uiOptions", "flags",
            "requiresSmallestWidthDp", "compatibleWidthLimitDp", "largestWidthLimitDp", "storageUuid", "sourceDir",
            "publicSourceDir", "splitNames", "splitSourceDirs", "splitPublicSourceDirs", "sharedLibraryFiles",
            "dataDir", "deviceProtectedDataDir", "nativeLibraryDir", "uid", "minSdkVersion", "targetSdkVersion",
            "compileSdkVersion", "compileSdkVersionCodename", "enabled", "appComponentFactory", "category"};
    private static final String[] ACTIVITY = {"theme", "launchMode", "documentLaunchMode", "persistableMode",
            "maxRecents", "permission", "taskAffinity", "targetActivity", "flags", "screenOrientation",
            "configChanges", "softInputMode", "uiOptions", "parentActivityName", "colorMode", "windowLayout",
            "requiredDisplayCategory"};
    private static final String[] SERVICE = {"permission", "flags"};
    private static final String[] PROVIDER = {"authority", "readPermission", "writePermission",
            "grantUriPermissions", "forceUriPermissions", "uriPermissionPatterns", "pathPermissions", "multiprocess",
            "initOrder", "flags", "isSyncable"};
    private static final String[] PACKAGE_FIELDS = {"packageName", "splitNames", "versionCode", "versionName",
            "baseRevisionCode", "splitRevisionCodes", "sharedUserId", "sharedUserLabel", "firstInstallTime",
            "lastUpdateTime", "installLocation", "isApex"};
    private static final String[] PERMISSION = {"protectionLevel", "group", "flags", "descriptionRes",
            "nonLocalizedDescription"};
    private static final String[] FEATURE = {"name", "version", "reqGlEsVersion", "flags"};
    private static final String[] CONFIGURATION = {"reqTouchScreen", "reqKeyboardType", "reqNavigation",
            "reqInputFeatures", "reqGlEsVersion"};
    /** Ints that are flags or resource ids, logged in hex. */
    private static final List<String> HEX = Arrays.asList("flags", "theme", "icon", "logo", "banner", "labelRes",
            "descriptionRes", "uiOptions", "configChanges", "softInputMode", "reqGlEsVersion", "version",
            "sharedUserLabel", "protectionLevel", "reqInputFeatures");

    private static long sFirstInstallTime;

    static {
        PackageManager pm = null;
        try {
            pm = new ApplicationPackageManager(ActivityThread.currentActivityThread());
        } catch (NoSuchMethodError e) {
            // Android: there is no such constructor, and onCreate reports.
        }
        if (pm != null) {
            run(pm);
        }
    }

    @Override
    public void onCreate() {
        super.onCreate();
        run(getPackageManager());
    }

    static void run(PackageManager pm) {
        try {
            report(pm);
        } catch (Throwable e) {
            Log.i(TAG, "failed: " + e);
        }
        Log.i(TAG, "done");
    }

    @SuppressWarnings("deprecation") // getPackageInfo(String, int), which apps below API 33 call.
    static void report(PackageManager pm) throws Exception {
        PackageInfo info = pm.getPackageInfo(PACKAGE, PackageManager.PackageInfoFlags.of(FLAGS));
        sFirstInstallTime = info.firstInstallTime;
        log("package", info, PACKAGE_FIELDS);
        Log.i(TAG, "longVersionCode " + info.getLongVersionCode());
        Log.i(TAG, "requestedPermissions " + Arrays.toString(info.requestedPermissions));
        application("application", info.applicationInfo);
        components("activity", info.activities, ACTIVITY);
        components("service", info.services, SERVICE);
        for (ServiceInfo service : info.services) {
            Log.i(TAG, "service " + service.name + " foregroundServiceType 0x"
                    + Integer.toHexString(service.getForegroundServiceType()));
        }
        components("provider", info.providers, PROVIDER);
        components("receiver", info.receivers, ACTIVITY);
        if (info.permissions != null) {
            for (PermissionInfo permission : info.permissions) {
                log("permission", permission, ITEM, PERMISSION);
            }
        }
        if (info.reqFeatures != null) {
            for (FeatureInfo feature : info.reqFeatures) {
                log("feature", feature, FEATURE);
            }
        }
        if (info.configPreferences != null) {
            for (ConfigurationInfo configuration : info.configPreferences) {
                log("configuration", configuration, CONFIGURATION);
            }
        }

        // Without flags: no components, permissions or meta-data.
        PackageInfo bare = pm.getPackageInfo(PACKAGE, 0);
        Log.i(TAG, "bare activities " + Arrays.toString(bare.activities) + ", permissions "
                + Arrays.toString(bare.permissions) + ", reqFeatures " + (bare.reqFeatures != null)
                + ", metaData " + bare.applicationInfo.metaData);
        application("getApplicationInfo", pm.getApplicationInfo(PACKAGE,
                PackageManager.ApplicationInfoFlags.of(PackageManager.GET_META_DATA)));
        Log.i(TAG, "label " + pm.getApplicationLabel(info.applicationInfo));

        ComponentName second = new ComponentName(PACKAGE, PACKAGE + ".SecondActivity");
        log("getActivityInfo", pm.getActivityInfo(second,
                PackageManager.ComponentInfoFlags.of(PackageManager.GET_META_DATA)), ITEM, COMPONENT, ACTIVITY);
        log("getActivityInfo alias", pm.getActivityInfo(new ComponentName(PACKAGE, PACKAGE + ".Alias"),
                PackageManager.ComponentInfoFlags.of(0)), ITEM, COMPONENT, ACTIVITY);
        log("getServiceInfo", pm.getServiceInfo(new ComponentName(PACKAGE, PACKAGE + ".TestService"),
                PackageManager.ComponentInfoFlags.of(0)), ITEM, COMPONENT, SERVICE);
        log("getProviderInfo", pm.getProviderInfo(new ComponentName(PACKAGE, PACKAGE + ".TestProvider"),
                PackageManager.ComponentInfoFlags.of(0)), ITEM, COMPONENT, PROVIDER);
        log("getReceiverInfo", pm.getReceiverInfo(new ComponentName(PACKAGE, PACKAGE + ".TestReceiver"),
                PackageManager.ComponentInfoFlags.of(0)), ITEM, COMPONENT, ACTIVITY);

        notFound(pm, "com.google.android.gms");
        notFound(pm, "com.android.vending");
        notFound(pm, "io.github.kisame76.minato.test.nothing");
        try {
            pm.getActivityInfo(new ComponentName(PACKAGE, PACKAGE + ".Missing"), 0);
            Log.i(TAG, "getActivityInfo Missing answered");
        } catch (PackageManager.NameNotFoundException e) {
            Log.i(TAG, "getActivityInfo Missing: NameNotFoundException " + e.getMessage());
        }
    }

    @SuppressWarnings("deprecation") // the int flags, as apps below API 33 pass them.
    private static void notFound(PackageManager pm, String name) {
        try {
            pm.getPackageInfo(name, 0);
            Log.i(TAG, "getPackageInfo " + name + " answered");
        } catch (PackageManager.NameNotFoundException e) {
            Log.i(TAG, "getPackageInfo " + name + ": NameNotFoundException " + e.getMessage());
        }
        try {
            pm.getApplicationInfo(name, 0);
            Log.i(TAG, "getApplicationInfo " + name + " answered");
        } catch (PackageManager.NameNotFoundException e) {
            Log.i(TAG, "getApplicationInfo " + name + ": NameNotFoundException " + e.getMessage());
        }
    }

    private static void application(String what, ApplicationInfo info) throws Exception {
        log(what, info, ITEM, APPLICATION);
        Log.i(TAG, what + " uid is myUid " + (info.uid == Process.myUid()));
    }

    private static void components(String what, ComponentInfo[] components, String[] fields) throws Exception {
        if (components == null) {
            Log.i(TAG, what + "s null");
            return;
        }
        for (ComponentInfo component : components) {
            log(what, component, ITEM, COMPONENT, fields);
        }
    }

    /** One line per object: its fields by name, in the given order. */
    private static void log(String what, Object object, String[]... names) throws Exception {
        List<String> parts = new ArrayList<>();
        for (String[] group : names) {
            for (String name : group) {
                Field field = field(object.getClass(), name);
                Object value = field.get(object);
                if (object instanceof ApplicationInfo && name.equals("flags")) {
                    // FLAG_STOPPED is the launch history's, not the manifest's: Android 15 clears it once the app
                    // has been bound after its first start, so a fresh install still has it here and a second start
                    // does not. Ours never sets it.
                    value = (Integer) value & ~ApplicationInfo.FLAG_STOPPED;
                }
                parts.add(name + "=" + value(name, value));
            }
        }
        Log.i(TAG, what + " " + String.join(", ", parts));
    }

    private static Field field(Class<?> type, String name) throws NoSuchFieldException {
        for (Class<?> c = type; c != null; c = c.getSuperclass()) {
            try {
                return c.getDeclaredField(name);
            } catch (NoSuchFieldException e) {
                // In a superclass.
            }
        }
        throw new NoSuchFieldException(type.getName() + "." + name);
    }

    private static String value(String name, Object value) {
        if (value == null) {
            return "null";
        }
        switch (name) {
            case "sourceDir":
            case "publicSourceDir":
                return codePath(value.toString(), "/base.apk");
            case "nativeLibraryDir":
                return codePath(value.toString(), "/lib/arm64");
            case "uid": {
                int uid = (Integer) value;
                // Process.FIRST_APPLICATION_UID and LAST_APPLICATION_UID, which step a's Process lacks.
                return uid >= 10000 && uid <= 19999 ? "<app uid>" : String.valueOf(uid);
            }
            case "firstInstallTime":
                return (Long) value > 0 ? "<time>" : value.toString();
            case "lastUpdateTime":
                return (Long) value >= sFirstInstallTime && sFirstInstallTime > 0
                        ? "<time, not before the first install>" : value.toString();
            default:
                break;
        }
        if (value instanceof Integer && HEX.contains(name)) {
            return "0x" + Integer.toHexString((Integer) value);
        }
        if (value instanceof ApplicationInfo) {
            return "<" + ((ApplicationInfo) value).packageName + ">";
        }
        if (value instanceof Bundle) {
            Bundle bundle = (Bundle) value;
            List<String> entries = new ArrayList<>();
            for (String key : new TreeSet<>(bundle.keySet())) {
                Object entry = bundle.get(key);
                entries.add(key + "=" + entry + (entry == null ? "" : " (" + entry.getClass().getSimpleName() + ")"));
            }
            return "{" + String.join(", ", entries) + "}";
        }
        if (value.getClass().isArray()) {
            List<String> items = new ArrayList<>();
            for (int i = 0; i < Array.getLength(value); i++) {
                items.add(String.valueOf(Array.get(value, i)));
            }
            return "[" + String.join(", ", items) + "]";
        }
        if (value instanceof String || value instanceof Number || value instanceof Boolean
                || value instanceof CharSequence || value instanceof UUID) {
            return value.toString().replace("\n", "\\n");
        }
        return "<" + value.getClass().getName() + ">";
    }

    /**
     * Decision 6's form, Android's: /data/app/~~<a>/<package>-<b><end>, where <a> and <b> are 16 bytes in URL-safe
     * Base64 with padding. Another shape is logged as it is, so the comparison fails.
     */
    private static String codePath(String path, String end) {
        String random = "[A-Za-z0-9_-]{22}==";
        if (path.matches("/data/app/~~" + random + "/" + PACKAGE.replace(".", "\\.") + "-" + random
                + end.replace(".", "\\."))) {
            return "/data/app/~~<a>/" + PACKAGE + "-<b>" + end;
        }
        return path;
    }
}
