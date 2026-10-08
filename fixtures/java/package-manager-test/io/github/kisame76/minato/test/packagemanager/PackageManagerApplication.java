package io.github.kisame76.minato.test.packagemanager;

import android.app.Application;
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
import android.content.pm.Signature;
import android.content.pm.SigningInfo;
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
 * package-manager-test). onCreate reports through getPackageManager(), on Minato since step c2b's ActivityThread
 * calls it, and ends with "done".
 *
 * Every public field of the SDK's info classes is logged, by name, since reflection on Android hides the hidden ones
 * and ours does not. What differs by decision is masked, after checking its shape: the code paths' random folders
 * (decision 6), the uid (decision 3), the install times and the application's FLAG_STOPPED.
 *
 * Since b3.5b1 also the runtime permissions' grants (ADR-0045 decision 6), the platform's permissions from
 * framework-res's manifest, with those behind a feature flag that is off left out, and the signatures. A certificate
 * is logged as its SHA-256, computed here, since Minato's guest has no JCA provider yet; the harnesses replace the
 * APK's own, which make-apk's per-machine debug key gives (ADR-0031), with a mark. hasSigningCertificate is asked
 * with the raw certificate only: its SHA-256 form digests through the JCA. The gids come later. Since gap 1's batch 2
 * also the installer of the app and of an unknown package.
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
    /**
     * The platform's permissions asked by name: normal, dangerous with restriction flags or an instant one, a
     * platform signature one, POST_NOTIFICATIONS, CAMERA_HEADLESS_SYSTEM_USER behind a flag that is on, and
     * QUARANTINE_APPS behind one that is off; then one nobody declares.
     */
    private static final String[] PLATFORM_PERMISSIONS = {"android.permission.INTERNET",
            "android.permission.CAMERA", "android.permission.READ_EXTERNAL_STORAGE",
            "android.permission.ACCESS_FINE_LOCATION", "android.permission.ACCESS_BACKGROUND_LOCATION",
            "android.permission.READ_LOGS", "android.permission.POST_NOTIFICATIONS",
            "android.permission.CAMERA_HEADLESS_SYSTEM_USER", "android.permission.QUARANTINE_APPS",
            "io.github.kisame76.minato.test.NOBODY"};
    /**
     * System features Minato reports as Android does (ADR-0045 decision 4); those it leaves out or reports in
     * another version are SystemFeaturesTest's, against the reference AVD's list. Then one no device has.
     */
    private static final String[] FEATURES_ASKED = {"android.hardware.audio.output", "android.hardware.faketouch",
            "android.hardware.ram.normal", "android.hardware.screen.landscape", "android.hardware.screen.portrait",
            "android.hardware.touchscreen", "android.hardware.touchscreen.multitouch.jazzhand",
            "android.software.app_enumeration", "io.github.kisame76.minato.test.none"};
    /** Ints that are flags or resource ids, logged in hex. */
    private static final List<String> HEX = Arrays.asList("flags", "theme", "icon", "logo", "banner", "labelRes",
            "descriptionRes", "uiOptions", "configChanges", "softInputMode", "reqGlEsVersion", "version",
            "sharedUserLabel", "protectionLevel", "reqInputFeatures");

    /** SHA-256's round constants. */
    private static final int[] K = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
    };

    private static long sFirstInstallTime;

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
            // Its stack, which differs by device, is only shown.
            Log.i(TAG + "-device", Log.getStackTraceString(e));
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
        StringBuilder requested = new StringBuilder();
        for (int flags : info.requestedPermissionsFlags) {
            requested.append(requested.length() == 0 ? "" : ", ").append("0x").append(Integer.toHexString(flags));
        }
        Log.i(TAG, "requestedPermissionsFlags [" + requested + "]");
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

        // b3.5b1: the grants, the platform's permissions and the signatures.
        for (String permission : info.requestedPermissions) {
            Log.i(TAG, "checkPermission " + permission + " " + pm.checkPermission(permission, PACKAGE));
        }
        Log.i(TAG, "checkPermission not requested " + pm.checkPermission("android.permission.RECORD_AUDIO", PACKAGE)
                + ", another package's " + pm.checkPermission("android.permission.INTERNET", "com.google.android.gms"));
        for (String name : PLATFORM_PERMISSIONS) {
            try {
                log("getPermissionInfo", pm.getPermissionInfo(name, 0), ITEM, PERMISSION);
            } catch (PackageManager.NameNotFoundException e) {
                Log.i(TAG, "getPermissionInfo " + name + ": NameNotFoundException " + e.getMessage());
            }
        }
        signatures(pm, bare);
        for (String feature : FEATURES_ASKED) {
            Log.i(TAG, "hasSystemFeature " + feature + " " + pm.hasSystemFeature(feature));
        }
        Log.i(TAG, "hasSystemFeature touchscreen in version 1 "
                + pm.hasSystemFeature(PackageManager.FEATURE_TOUCHSCREEN, 1));
        try {
            pm.getActivityInfo(new ComponentName(PACKAGE, PACKAGE + ".Missing"), 0);
            Log.i(TAG, "getActivityInfo Missing answered");
        } catch (PackageManager.NameNotFoundException e) {
            Log.i(TAG, "getActivityInfo Missing: NameNotFoundException " + e.getMessage());
        }
        Log.i(TAG, "installer " + installer(pm, PACKAGE) + ", of an unknown package "
                + installer(pm, PACKAGE + ".none"));
    }

    @SuppressWarnings("deprecation") // getInstallerPackageName, which apps still call.
    private static String installer(PackageManager pm, String name) {
        try {
            return pm.getInstallerPackageName(name);
        } catch (IllegalArgumentException e) {
            return "IllegalArgumentException " + e.getMessage();
        }
    }

    @SuppressWarnings("deprecation") // GET_SIGNATURES, which apps still ask for.
    private static void signatures(PackageManager pm, PackageInfo bare) throws Exception {
        PackageInfo signed = pm.getPackageInfo(PACKAGE,
                PackageManager.GET_SIGNATURES | PackageManager.GET_SIGNING_CERTIFICATES);
        Log.i(TAG, "signatures " + digests(signed.signatures));
        SigningInfo signing = signed.signingInfo;
        Log.i(TAG, "signingInfo " + (signing == null ? "null" : "hasMultipleSigners=" + signing.hasMultipleSigners()
                + ", hasPastSigningCertificates=" + signing.hasPastSigningCertificates()
                + ", apkContentsSigners=" + digests(signing.getApkContentsSigners())
                + ", signingCertificateHistory=" + digests(signing.getSigningCertificateHistory())));
        Log.i(TAG, "without the flags signatures " + Arrays.toString(bare.signatures) + ", signingInfo "
                + bare.signingInfo);
        Log.i(TAG, "checkSignatures itself " + pm.checkSignatures(PACKAGE, PACKAGE) + ", Play services "
                + pm.checkSignatures(PACKAGE, "com.google.android.gms") + ", its uid "
                + pm.checkSignatures(Process.myUid(), Process.myUid()));
        byte[] certificate = signed.signatures[0].toByteArray();
        Log.i(TAG, "hasSigningCertificate itself " + pm.hasSigningCertificate(PACKAGE, certificate,
                PackageManager.CERT_INPUT_RAW_X509) + ", another certificate " + pm.hasSigningCertificate(PACKAGE,
                new byte[] {0x30, 0x00}, PackageManager.CERT_INPUT_RAW_X509) + ", its uid "
                + pm.hasSigningCertificate(Process.myUid(), certificate, PackageManager.CERT_INPUT_RAW_X509)
                + ", Play services " + pm.hasSigningCertificate("com.google.android.gms", certificate,
                PackageManager.CERT_INPUT_RAW_X509));
    }

    /** Each certificate's SHA-256 in hex, in a list, or null. */
    private static String digests(Signature[] signatures) {
        if (signatures == null) {
            return "null";
        }
        List<String> digests = new ArrayList<>();
        for (Signature signature : signatures) {
            StringBuilder hex = new StringBuilder();
            for (byte b : sha256(signature.toByteArray())) {
                hex.append(String.format("%02x", b));
            }
            digests.add(hex.toString());
        }
        return digests.toString();
    }

    /** SHA-256 (FIPS 180-4), here because Minato's guest has no JCA provider yet. */
    static byte[] sha256(byte[] message) {
        int[] h = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
        int length = ((message.length + 8) / 64 + 1) * 64;
        byte[] data = Arrays.copyOf(message, length);
        data[message.length] = (byte) 0x80;
        long bits = (long) message.length * 8;
        for (int i = 0; i < 8; i++) {
            data[length - 1 - i] = (byte) (bits >>> (8 * i));
        }
        int[] w = new int[64];
        for (int block = 0; block < length; block += 64) {
            for (int t = 0; t < 16; t++) {
                int at = block + 4 * t;
                w[t] = (data[at] & 0xff) << 24 | (data[at + 1] & 0xff) << 16 | (data[at + 2] & 0xff) << 8
                        | (data[at + 3] & 0xff);
            }
            for (int t = 16; t < 64; t++) {
                int s0 = Integer.rotateRight(w[t - 15], 7) ^ Integer.rotateRight(w[t - 15], 18) ^ (w[t - 15] >>> 3);
                int s1 = Integer.rotateRight(w[t - 2], 17) ^ Integer.rotateRight(w[t - 2], 19) ^ (w[t - 2] >>> 10);
                w[t] = w[t - 16] + s0 + w[t - 7] + s1;
            }
            int a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
            for (int t = 0; t < 64; t++) {
                int t1 = k + (Integer.rotateRight(e, 6) ^ Integer.rotateRight(e, 11) ^ Integer.rotateRight(e, 25))
                        + ((e & f) ^ (~e & g)) + K[t] + w[t];
                int t2 = (Integer.rotateRight(a, 2) ^ Integer.rotateRight(a, 13) ^ Integer.rotateRight(a, 22))
                        + ((a & b) ^ (a & c) ^ (b & c));
                k = g;
                g = f;
                f = e;
                e = d + t1;
                d = c;
                c = b;
                b = a;
                a = t1 + t2;
            }
            h[0] += a;
            h[1] += b;
            h[2] += c;
            h[3] += d;
            h[4] += e;
            h[5] += f;
            h[6] += g;
            h[7] += k;
        }
        byte[] digest = new byte[32];
        for (int i = 0; i < 32; i++) {
            digest[i] = (byte) (h[i / 4] >>> (24 - 8 * (i % 4)));
        }
        return digest;
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
