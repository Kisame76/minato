/*
 * Ours (ADR-0039 decision 7, b3.5a): the app's own package as its manifest declares it, read once by ActivityThread as
 * the app starts, from which ApplicationPackageManager answers. The manifest is read as Android 15's package parser
 * reads it (ParsingPackageUtils and the Parsed*Utils of com.android.internal.pm, which decision 7 does not take):
 * through libandroidfw's XmlResourceParser, with every attribute through Resources.obtainAttributes and framework-res's
 * AndroidManifest* styleables, so defaults, references and value types are framework-res's. The info objects are made
 * per request as PackageInfoCommonUtils makes them, with the flags' filtering; what system_server adds at the install
 * (paths, uid, install times, FLAG_INSTALLED of the app and of its permissions) comes from the driver.
 *
 * Since b3.5b1 it also reads, as Android 15 does:
 * <ul>
 *   <li>the platform's permissions, from framework-res's manifest as the package "android", once;</li>
 *   <li>the split permissions of /system/etc/permissions/platform.xml, which add implicit permissions below their
 *       targetSdk (SystemConfig and ParsingPackageUtils.convertSplitPermissions);</li>
 *   <li>android:featureFlag on every element, with the values framework-res was built with
 *       (AconfigFlags.skipCurrentElement, ManifestFeatureFlags).</li>
 * </ul>
 * The runtime permissions' answers are PermissionPolicy's (ADR-0045 decision 6).
 *
 * Who signed the app comes from the import's signers.json (ADR-0034 decision 2), as SigningDetails.
 *
 * Not read yet, each for the step that needs it: intent filters beyond whether there are any (b3.5b2),
 * <uses-library> (sharedLibraryFiles), <layout>, <property>, <grant-uri-permission>, <path-permission>,
 * <permission-tree> and splits.
 */
package android.app;

import android.content.ComponentName;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.ComponentInfo;
import android.content.pm.ConfigurationInfo;
import android.content.pm.FeatureInfo;
import android.content.pm.InstrumentationInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageItemInfo;
import android.content.pm.PackageManager;
import android.content.pm.PathPermission;
import android.content.pm.PermissionGroupInfo;
import android.content.pm.PermissionInfo;
import android.content.pm.ProviderInfo;
import android.content.pm.ResolveInfo;
import android.content.pm.ServiceInfo;
import android.content.pm.SigningDetails;
import android.content.res.ApkAssets;
import android.content.res.AssetManager;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.content.res.TypedArray;
import android.content.res.XmlResourceParser;
import android.os.Bundle;
import android.os.PatternMatcher;
import android.os.Process;
import android.os.SystemProperties;
import android.util.Slog;
import android.util.SparseArray;
import android.util.TypedValue;
import android.util.Xml;
import com.android.internal.R;
import com.android.minato.permissions.PermissionPolicy;
import com.android.minato.pm.SignersFile;
import com.android.minato.pm.SystemFeatures;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.IdentityHashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Set;
import java.util.UUID;
import java.util.function.ToIntFunction;
import org.xmlpull.v1.XmlPullParser;
import org.xmlpull.v1.XmlPullParserException;

// Deprecated attributes, fields and flags: Android's parser still reads and fills them, and apps still read them.
@SuppressWarnings("deprecation")
final class ManifestPackage {
    /** Build.VERSION.SDK_INT of Android 15, against which <uses-permission>'s SDK bounds are checked. */
    private static final int SDK = 35;
    /** ActivityTaskManager.getDefaultAppRecentsLimitStatic() on a device that is not low on RAM: 48 / 6. */
    private static final int DEFAULT_MAX_RECENTS = 8;
    /** StorageManager.UUID_DEFAULT, internal storage. */
    private static final UUID UUID_DEFAULT = UUID.fromString("41217664-9172-527a-b3d5-edabb50a7d69");
    private static final String META_LAUNCH_MODE = "android.activity.launch_mode";
    private static final String META_REMOTE_DEVICES = "android.can_display_on_remote_devices";
    private static final String ANDROID_NAMESPACE = "http://schemas.android.com/apk/res/android";
    /** The platform's package, framework-res. */
    private static final String PLATFORM = "android";
    /** framework-res's folder, which AssetManager's system set holds (decision 8). */
    private static final String FRAMEWORK_RES = "/system/framework/framework-res";
    /**
     * The overlays of framework-res that Android 15's package manager gives every package's overlay paths: SystemUI's
     * Monet overlays, as the reference AVD names and orders them (ADR-0043 f)). tools/monet-overlays.py writes them
     * into /data/resource-cache from framework/monet-overlays.txt.
     */
    private static final String[] FRAMEWORK_OVERLAYS = {
            "/data/resource-cache/com.android.systemui-neutral-ridM.frro",
            "/data/resource-cache/com.android.systemui-accent-eYU1.frro",
            "/data/resource-cache/com.android.systemui-dynamic-Mh0R.frro"};
    /** SystemConfig's file of the platform's permissions, of which the guest reads the split permissions. */
    private static final String PLATFORM_XML = "/system/etc/permissions/platform.xml";
    /** SystemConfig's targetSdk of a split that names none: CUR_DEVELOPMENT + 1, so it always applies. */
    private static final int SPLIT_ALWAYS = 10001;
    private static final String POLICY_TAG = "PermissionPolicy";

    /** The platform's permissions by name, read once on first use. */
    private static Map<String, PermissionInfo> sPlatformPermissions;
    /** framework-res's permission groups by name, read with its permissions. */
    private static Map<String, PermissionGroupInfo> sPlatformGroups;
    /** framework-res's protected broadcasts, read with its permissions (ADR-0045 decision 5, step d2). */
    private static Set<String> sProtectedBroadcasts;
    /** platform.xml's split permissions in its order, read once by the first manifest. */
    private static List<Split> sSplits;

    /** A <split-permission>: below targetSdk, requesting permission requests newPermissions too. */
    private static final class Split {
        final String permission;
        final List<String> newPermissions;
        final int targetSdk;

        Split(String permission, List<String> newPermissions, int targetSdk) {
            this.permission = permission;
            this.newPermissions = newPermissions;
            this.targetSdk = targetSdk;
        }
    }

    final String packageName;
    /** The app's Resources over base.apk and framework-res, with the system's configuration. */
    /** Over base.apk, and over the splits too once splits() has read a set (as DefaultSplitAssetLoader's). */
    Resources resources;

    private final PackageInfo mPackage = new PackageInfo();
    private final ApplicationInfo mApplication = new ApplicationInfo();
    private final List<ActivityInfo> mActivities = new ArrayList<>();
    private final List<ActivityInfo> mReceivers = new ArrayList<>();
    /** The activities and aliases with a filter a home screen's launch intent matches, in manifest order. */
    private final List<ActivityInfo> mLaunchers = new ArrayList<>();
    private final List<ServiceInfo> mServices = new ArrayList<>();
    private final List<ProviderInfo> mProviders = new ArrayList<>();
    // Each component's intent filters as the package parser keeps them (step SV1), in manifest order.
    private final Map<ComponentInfo, List<IntentFilter>> mFilters = new IdentityHashMap<>();
    private final List<PermissionInfo> mPermissions = new ArrayList<>();
    private final List<PermissionGroupInfo> mPermissionGroups = new ArrayList<>();
    private final List<InstrumentationInfo> mInstrumentations = new ArrayList<>();
    private final List<String> mRequestedPermissions = new ArrayList<>();
    /** Each requested permission's android:usesPermissionFlags; 0 for an implicit one. */
    private final Map<String, Integer> mUsesPermissionFlags = new HashMap<>();
    /** The permissions added by compatibility and split permissions (ParsingPackage.getImplicitPermissions). */
    private final Set<String> mImplicitPermissions = new HashSet<>();
    /** Who signed the app, from the import's signers.json; UNKNOWN for an unsigned one. */
    private SigningDetails mSigningDetails = SigningDetails.UNKNOWN;
    /** Whether the missing X.509 provider was logged, once per process. */
    private static boolean sX509Logged;
    /** Denials already logged, each once. */
    private final Set<String> mLoggedDenials = new HashSet<>();
    /** Whether a departure from Android's grants was logged, once per app. */
    private boolean mDepartureLogged;
    private PermissionPolicy.App mPolicyApp;
    private final List<FeatureInfo> mFeatures = new ArrayList<>();
    private final List<ConfigurationInfo> mConfigurations = new ArrayList<>();

    // <supports-screens>: 1 is unset, as PackageImpl keeps them.
    private int mSmallScreens = 1, mNormalScreens = 1, mLargeScreens = 1, mXlargeScreens = 1, mResizeable = 1,
            mAnyDensity = 1;

    private ManifestPackage(String packageName, Resources resources) {
        this.packageName = packageName;
        this.resources = resources;
    }

    /**
     * Reads base.apk's manifest; the folders and the install time are the install's, as decision 6 lays them out.
     */
    static ManifestPackage read(String apk, String libraryDir, String dataDir)
            throws IOException, XmlPullParserException {
        return read(apk, libraryDir, dataDir, null);
    }

    /**
     * With the signers the import wrote (ADR-0034 decision 2, b3.5b1): a broken signers.json stops the app's start
     * (SignersFile); null for an unsigned app.
     */
    static ManifestPackage read(String apk, String libraryDir, String dataDir, String signers)
            throws IOException, XmlPullParserException {
        return read(apk, libraryDir, dataDir, signers, null);
    }

    /**
     * With the folder GuestStart names when the library keeps fast-follow or on-demand pack slices of the app, laid out
     * under the app's external files folder (ADR-0050 decision 2): the application's meta-data local_testing_dir names
     * it, which switches Google's Play libraries to their local-testing path, unless the base's manifest has its own;
     * null for an app without packs.
     */
    static ManifestPackage read(String apk, String libraryDir, String dataDir, String signers, String localTestingDir)
            throws IOException, XmlPullParserException {
        return read(apk, libraryDir, dataDir, signers, localTestingDir, new String[0]);
    }

    /**
     * With the splits of an installed set, in the order GuestStart names them (ADR-0048 decision 9): each split's
     * manifest gives its name, and the package lists them in string order with their paths, as Android 15 does.
     */
    static ManifestPackage read(String apk, String libraryDir, String dataDir, String signers, String localTestingDir,
            String[] splits) throws IOException, XmlPullParserException {
        ApkAssets apkAssets = ApkAssets.loadFromPath(apk);
        AssetManager assets = new AssetManager();
        assets.setApkAssets(new ApkAssets[] {apkAssets}, false);
        Resources system = Resources.getSystem();
        Resources res = new Resources(assets, system.getDisplayMetrics(), system.getConfiguration());
        try (XmlResourceParser parser = assets.openXmlResourceParser(assets.findCookieForPath(apk),
                "AndroidManifest.xml")) {
            ManifestPackage pkg = new ManifestPackage(manifestElement(parser, apk), res);
            pkg.parseManifest(parser);
            pkg.install(apk, libraryDir, dataDir);
            if (signers != null) {
                pkg.mSigningDetails = PackageSigning.details(SignersFile.read(new File(signers)));
            }
            if (localTestingDir != null) {
                pkg.localTesting(localTestingDir);
            }
            pkg.splits(splits);
            return pkg;
        }
    }

    /**
     * The splits' names from their manifests' split attribute, and the lists Android 15 sets for an installed set:
     * ApplicationInfo's splitNames with splitSourceDirs and splitPublicSourceDirs in the same, string order, and
     * PackageInfo's splitNames with their revision codes.
     */
    private void splits(String[] paths) throws IOException, XmlPullParserException {
        if (paths.length == 0) {
            return;
        }
        String[] names = new String[paths.length];
        int[] revisions = new int[paths.length];
        String[] configFor = new String[paths.length];
        String[] usesSplit = new String[paths.length];
        boolean[] feature = new boolean[paths.length];
        Integer[] order = new Integer[paths.length];
        for (int i = 0; i < paths.length; i++) {
            AssetManager assets = new AssetManager();
            assets.setApkAssets(new ApkAssets[] {ApkAssets.loadFromPath(paths[i])}, false);
            try (XmlResourceParser parser = assets.openXmlResourceParser(assets.findCookieForPath(paths[i]),
                    "AndroidManifest.xml")) {
                manifestElement(parser, paths[i]);
                names[i] = parser.getAttributeValue(null, "split");
                revisions[i] = parser.getAttributeIntValue(ANDROID_NAMESPACE, "revisionCode", 0);
                configFor[i] = parser.getAttributeValue(null, "configForSplit");
                feature[i] = parser.getAttributeBooleanValue(ANDROID_NAMESPACE, "isFeatureSplit", false);
                int depth = parser.getDepth();
                int type;
                while ((type = parser.next()) != XmlPullParser.END_DOCUMENT
                        && (type != XmlPullParser.END_TAG || parser.getDepth() > depth)) {
                    if (type == XmlPullParser.START_TAG && parser.getDepth() == depth + 1
                            && "uses-split".equals(parser.getName())) {
                        usesSplit[i] = parser.getAttributeValue(ANDROID_NAMESPACE, "name");
                    }
                }
            } finally {
                assets.close();
            }
            if (names[i] == null) {
                throw new IOException(paths[i] + "'s manifest names no split");
            }
            order[i] = i;
        }
        Arrays.sort(order, (x, y) -> names[x].compareTo(names[y]));
        mPackage.splitNames = new String[paths.length];
        mPackage.splitRevisionCodes = new int[paths.length];
        mApplication.splitNames = new String[paths.length];
        mApplication.splitSourceDirs = new String[paths.length];
        for (int i = 0; i < paths.length; i++) {
            mPackage.splitNames[i] = names[order[i]];
            mPackage.splitRevisionCodes[i] = revisions[order[i]];
            mApplication.splitNames[i] = names[order[i]];
            mApplication.splitSourceDirs[i] = paths[order[i]];
        }
        mApplication.splitPublicSourceDirs = mApplication.splitSourceDirs.clone();
        // What a split depends on: the split a configuration split configures, else the one it uses, else the base
        // (ParsingPackageUtils.buildSplitDependencies); the dependencies and the feature splits are kept for isolated
        // loading (ApplicationInfo.splitDependencies holds them as Android's does, index 0 the base).
        mFeatureSplits = new boolean[paths.length];
        SparseArray<int[]> dependencies = new SparseArray<>();
        dependencies.put(0, new int[] {-1});
        for (int i = 0; i < paths.length; i++) {
            String target = configFor[order[i]] != null ? configFor[order[i]] : usesSplit[order[i]];
            int parent = target == null ? 0 : 1 + Arrays.asList(mApplication.splitNames).indexOf(target);
            if (target != null && parent == 0) {
                // Android refuses such a set at install ("requires split ... which is missing"); the library
                // checks sets at import, so here it only shows in the log and the base stays the parent.
                Slog.w("PackageManager", "Split " + names[order[i]] + " requires split " + target + ", not installed");
            }
            mFeatureSplits[i] = feature[order[i]];
            dependencies.put(1 + i, new int[] {parent});
        }
        if (isolatedSplits()) {
            mApplication.splitDependencies = dependencies;
        }
        // The splits' components follow the base's, in the splits' order, each with its split's name. Their attributes
        // resolve against the base and all the splits, as the package parser takes a split's from the set's
        // DefaultSplitAssetLoader; these Resources stay the package's.
        ApkAssets[] set = new ApkAssets[1 + paths.length];
        set[0] = ApkAssets.loadFromPath(mApplication.sourceDir);
        for (int i = 0; i < paths.length; i++) {
            set[1 + i] = ApkAssets.loadFromPath(mApplication.splitSourceDirs[i]);
        }
        AssetManager assets = new AssetManager();
        assets.setApkAssets(set, false);
        Resources system = Resources.getSystem();
        resources = new Resources(assets, system.getDisplayMetrics(), system.getConfiguration());
        for (int i = 0; i < paths.length; i++) {
            splitComponents(assets, mApplication.splitSourceDirs[i], mApplication.splitNames[i]);
        }
    }

    /**
     * What a split's manifest adds to the package (ParsingPackageUtils.parseSplitApplication): its application's
     * activities, receivers, aliases, services and providers, each with the split's name, and its meta-data, merged
     * into the application's. A component the base declares too is kept as well, so the lookup by name finds the later
     * one [verified on Android 15, splits-test].
     */
    private void splitComponents(AssetManager assets, String path, String split)
            throws IOException, XmlPullParserException {
        int activities = mActivities.size(), receivers = mReceivers.size(), services = mServices.size();
        int providers = mProviders.size();
        try (XmlResourceParser parser = assets.openXmlResourceParser(assets.findCookieForPath(path),
                "AndroidManifest.xml")) {
            manifestElement(parser, path);
            int depth = parser.getDepth();
            int type;
            while ((type = parser.next()) != XmlPullParser.END_DOCUMENT
                    && (type != XmlPullParser.END_TAG || parser.getDepth() > depth)) {
                if (type == XmlPullParser.START_TAG && parser.getDepth() == depth + 1 && !skipped(parser)
                        && "application".equals(parser.getName())) {
                    splitApplication(parser);
                }
            }
        }
        for (ActivityInfo info : mActivities.subList(activities, mActivities.size())) {
            info.splitName = split;
        }
        for (ActivityInfo info : mReceivers.subList(receivers, mReceivers.size())) {
            info.splitName = split;
        }
        for (ServiceInfo info : mServices.subList(services, mServices.size())) {
            info.splitName = split;
        }
        for (ProviderInfo info : mProviders.subList(providers, mProviders.size())) {
            info.splitName = split;
        }
    }

    /** The children of a split's <application> that a split adds. */
    private void splitApplication(XmlResourceParser parser) throws IOException, XmlPullParserException {
        int depth = parser.getDepth();
        int type;
        while ((type = parser.next()) != XmlPullParser.END_DOCUMENT
                && (type != XmlPullParser.END_TAG || parser.getDepth() > depth)) {
            if (type != XmlPullParser.START_TAG || parser.getDepth() != depth + 1 || skipped(parser)) {
                continue;
            }
            switch (parser.getName()) {
                case "meta-data": mApplication.metaData = metaData(parser, mApplication.metaData); break;
                case "activity": mActivities.add(parseActivity(parser, false)); break;
                case "receiver": mReceivers.add(parseActivity(parser, true)); break;
                case "activity-alias": mActivities.add(parseAlias(parser)); break;
                case "service": mServices.add(parseService(parser)); break;
                case "provider": mProviders.add(parseProvider(parser)); break;
                default: break;
            }
        }
    }

    private boolean[] mFeatureSplits;

    boolean isolatedSplits() {
        return mApplication.requestsIsolatedSplitLoading();
    }

    /** Whether each split, in splitNames' order, is a feature split (the rest configure one), else null. */
    boolean[] featureSplits() {
        return mFeatureSplits;
    }

    /** The class path of the app's loader: base.apk, then the splits, as Android's PathClassLoader over the set. */
    String codePaths() {
        StringBuilder paths = new StringBuilder(mApplication.sourceDir);
        // A base that asks for isolated splits gets a loader of its own code only; each split has one (LoadedApk).
        if (mApplication.splitSourceDirs != null && !isolatedSplits()) {
            for (String split : mApplication.splitSourceDirs) {
                paths.append(File.pathSeparatorChar).append(split);
            }
        }
        return paths.toString();
    }

    /**
     * The meta-data local_testing_dir = `folder`, where the base's manifest names none (ADR-0050 decision 2), as
     * bundletool's build-apks --local-testing adds it to a base that lacks it; getApplicationInfo gives it with
     * GET_META_DATA, as it gives the manifest's.
     */
    private void localTesting(String folder) {
        if (mApplication.metaData == null) {
            mApplication.metaData = new Bundle();
        }
        if (!mApplication.metaData.containsKey("local_testing_dir")) {
            mApplication.metaData.putString("local_testing_dir", folder);
        }
    }

    /** Moves to the manifest's first element, <manifest>, and returns its package. */
    private static String manifestElement(XmlResourceParser parser, String where)
            throws IOException, XmlPullParserException {
        int type;
        while ((type = parser.next()) != XmlPullParser.START_TAG && type != XmlPullParser.END_DOCUMENT) {
            // The manifest's first element.
        }
        if (type != XmlPullParser.START_TAG || !"manifest".equals(parser.getName())) {
            throw new XmlPullParserException(where + ": no <manifest>");
        }
        String name = parser.getAttributeValue(null, "package");
        if (name == null || name.isEmpty()) {
            throw new XmlPullParserException(where + ": <manifest> has no package");
        }
        return name;
    }

    /** Who signed the app, for checkSignatures and hasSigningCertificate. */
    SigningDetails signingDetails() {
        return mSigningDetails;
    }

    /**
     * The platform's permissions: framework-res's manifest read once as the package "android", as Android's package
     * manager reads it at boot, with FLAG_INSTALLED as its permissions are installed. Only <permission> is read, and
     * <protected-broadcast>'s names, which isProtectedBroadcast answers from (ADR-0045 decision 5, step d2).
     */
    private static synchronized Map<String, PermissionInfo> platformPermissions() {
        if (sPlatformPermissions == null) {
            AssetManager assets = AssetManager.getSystem();
            ManifestPackage platform;
            Set<String> protectedBroadcasts = new HashSet<>();
            try (XmlResourceParser parser = assets.openXmlResourceParser(assets.findCookieForPath(FRAMEWORK_RES),
                    "AndroidManifest.xml")) {
                platform = new ManifestPackage(manifestElement(parser, FRAMEWORK_RES), Resources.getSystem());
                if (!PLATFORM.equals(platform.packageName)) {
                    throw new XmlPullParserException(FRAMEWORK_RES + " is " + platform.packageName + ", not android");
                }
                int depth = parser.getDepth();
                int type;
                while ((type = parser.next()) != XmlPullParser.END_DOCUMENT
                        && (type != XmlPullParser.END_TAG || parser.getDepth() > depth)) {
                    if (type != XmlPullParser.START_TAG || parser.getDepth() != depth + 1 || skipped(parser)) {
                        continue;
                    }
                    if ("permission".equals(parser.getName())) {
                        platform.parsePermission(parser);
                    } else if ("permission-group".equals(parser.getName())) {
                        platform.parsePermissionGroup(parser);
                    } else if ("protected-broadcast".equals(parser.getName())) {
                        // As ParsingPackageUtils.parseProtectedBroadcast: its name, intern'd, when it has one.
                        TypedArray sa = platform.resources.obtainAttributes(parser,
                                R.styleable.AndroidManifestProtectedBroadcast);
                        try {
                            String name = sa.getNonResourceString(R.styleable.AndroidManifestProtectedBroadcast_name);
                            if (name != null) {
                                protectedBroadcasts.add(name.intern());
                            }
                        } finally {
                            sa.recycle();
                        }
                    }
                }
            } catch (IOException | XmlPullParserException e) {
                throw new IllegalStateException("cannot read the platform's permissions from " + FRAMEWORK_RES, e);
            }
            Map<String, PermissionInfo> permissions = new HashMap<>();
            for (PermissionInfo permission : platform.mPermissions) {
                permission.flags |= PermissionInfo.FLAG_INSTALLED;
                permissions.put(permission.name, permission);
            }
            sPlatformPermissions = Collections.unmodifiableMap(permissions);
            Map<String, PermissionGroupInfo> groups = new LinkedHashMap<>();
            for (PermissionGroupInfo group : platform.mPermissionGroups) {
                groups.put(group.name, group);
            }
            sPlatformGroups = Collections.unmodifiableMap(groups);
            sProtectedBroadcasts = Collections.unmodifiableSet(protectedBroadcasts);
        }
        return sPlatformPermissions;
    }

    /**
     * Whether `action` is a protected broadcast of framework-res's manifest, which only the system may send, as the
     * package manager's isProtectedBroadcast answers (ADR-0045 decision 5, step d2); null is none.
     */
    static boolean isProtectedBroadcast(String action) {
        synchronized (ManifestPackage.class) {
            platformPermissions();
            return action != null && sProtectedBroadcasts.contains(action);
        }
    }

    /**
     * platform.xml's split permissions, as SystemConfig.readSplitPermission reads each <split-permission>: its name,
     * its targetSdk (always, when it names none) and its <new-permission>s; one without a name, with a targetSdk
     * that is not a number, or without a new permission is left out. A missing file gives no splits with a warning,
     * as SystemConfig skips a missing file; a broken one stops the app's start.
     */
    private static synchronized List<Split> splits() {
        if (sSplits == null) {
            List<Split> splits = new ArrayList<>();
            if (!new File(PLATFORM_XML).isFile()) {
                Slog.w("SystemConfig", "No split permissions: " + PLATFORM_XML + " is missing");
                sSplits = Collections.emptyList();
                return sSplits;
            }
            try (InputStream in = new FileInputStream(PLATFORM_XML)) {
                XmlPullParser parser = Xml.newPullParser();
                parser.setInput(in, null);
                int type;
                while ((type = parser.next()) != XmlPullParser.END_DOCUMENT) {
                    if (type != XmlPullParser.START_TAG || parser.getDepth() != 2
                            || !"split-permission".equals(parser.getName())) {
                        continue;
                    }
                    String permission = parser.getAttributeValue(null, "name");
                    String target = parser.getAttributeValue(null, "targetSdk");
                    int targetSdk = SPLIT_ALWAYS;
                    boolean valid = permission != null;
                    if (target != null && !target.isEmpty()) {
                        try {
                            targetSdk = Integer.parseInt(target);
                        } catch (NumberFormatException e) {
                            valid = false;
                        }
                    }
                    List<String> newPermissions = new ArrayList<>();
                    int depth = parser.getDepth();
                    while ((type = parser.next()) != XmlPullParser.END_DOCUMENT
                            && (type != XmlPullParser.END_TAG || parser.getDepth() > depth)) {
                        if (type == XmlPullParser.START_TAG && parser.getDepth() == depth + 1
                                && "new-permission".equals(parser.getName())) {
                            String name = parser.getAttributeValue(null, "name");
                            if (name != null && !name.isEmpty()) {
                                newPermissions.add(name);
                            }
                        }
                    }
                    if (valid && !newPermissions.isEmpty()) {
                        splits.add(new Split(permission, Collections.unmodifiableList(newPermissions), targetSdk));
                    }
                }
            } catch (IOException | XmlPullParserException e) {
                throw new IllegalStateException("cannot read the split permissions of " + PLATFORM_XML, e);
            }
            sSplits = Collections.unmodifiableList(splits);
        }
        return sSplits;
    }

    /**
     * AconfigFlags.skipCurrentElement: an element whose android:featureFlag is off ("!flag": on) is left out, and so
     * is one whose flag is unknown, with ap3a's values (ManifestFeatureFlags) where Android reads the device's.
     */
    private static boolean skipped(XmlResourceParser parser) {
        if (!android.content.res.Flags.manifestFlagging()) {
            return false;
        }
        String flag = parser.getAttributeValue(ANDROID_NAMESPACE, "featureFlag");
        if (flag == null) {
            return false;
        }
        // AconfigFlags strips; trim is the same for a flag's name, and D8 warns about strip below API 33.
        flag = flag.trim();
        boolean negated = flag.startsWith("!");
        if (negated) {
            flag = flag.substring(1).trim();
        }
        int index = Arrays.binarySearch(ManifestFeatureFlags.NAMES, flag);
        if (index < 0) {
            Slog.w("AconfigFlags", "Skipping element " + parser.getName() + " due to unknown feature flag " + flag);
            return true;
        }
        return ManifestFeatureFlags.VALUES[index] == negated;
    }

    // The manifest.

    private void parseManifest(XmlResourceParser parser) throws IOException, XmlPullParserException {
        TypedArray sa = resources.obtainAttributes(parser, R.styleable.AndroidManifest);
        try {
            mPackage.packageName = packageName;
            mPackage.versionCode = sa.getInteger(R.styleable.AndroidManifest_versionCode, 0);
            mPackage.versionCodeMajor = sa.getInteger(R.styleable.AndroidManifest_versionCodeMajor, 0);
            mPackage.versionName = sa.getNonConfigurationString(R.styleable.AndroidManifest_versionName, 0);
            mPackage.baseRevisionCode = sa.getInteger(R.styleable.AndroidManifest_revisionCode, 0);
            mPackage.installLocation = sa.getInteger(R.styleable.AndroidManifest_installLocation,
                    PackageInfo.INSTALL_LOCATION_UNSPECIFIED);
            if (sa.getBoolean(R.styleable.AndroidManifest_isolatedSplits, false)) {
                mApplication.privateFlags |= ApplicationInfo.PRIVATE_FLAG_ISOLATED_SPLIT_LOADING;
            }
            String sharedUserId = sa.getNonConfigurationString(R.styleable.AndroidManifest_sharedUserId, 0);
            if (sharedUserId != null && !sharedUserId.isEmpty()) {
                mPackage.sharedUserId = sharedUserId;
                mPackage.sharedUserLabel = sa.getResourceId(R.styleable.AndroidManifest_sharedUserLabel, 0);
            }
            mApplication.compileSdkVersion = sa.getInteger(R.styleable.AndroidManifest_compileSdkVersion, 0);
            mApplication.compileSdkVersionCodename = sa.getNonConfigurationString(
                    R.styleable.AndroidManifest_compileSdkVersionCodename, 0);
        } finally {
            sa.recycle();
        }
        mPackage.splitNames = new String[0];
        mPackage.splitRevisionCodes = new int[0];
        mApplication.packageName = packageName;
        // <uses-sdk> absent: min 1, target 0.
        mApplication.minSdkVersion = 1;
        mApplication.targetSdkVersion = 0;

        int depth = parser.getDepth();
        int type;
        while ((type = parser.next()) != XmlPullParser.END_DOCUMENT
                && (type != XmlPullParser.END_TAG || parser.getDepth() > depth)) {
            if (type != XmlPullParser.START_TAG || parser.getDepth() != depth + 1 || skipped(parser)) {
                continue;
            }
            switch (parser.getName()) {
                case "uses-sdk": parseUsesSdk(parser); break;
                case "uses-permission":
                case "uses-permission-sdk-m":
                case "uses-permission-sdk-23": parseUsesPermission(parser); break;
                case "permission": parsePermission(parser); break;
                case "permission-group": parsePermissionGroup(parser); break;
                case "instrumentation": parseInstrumentation(parser); break;
                case "uses-feature": parseUsesFeature(parser); break;
                case "uses-configuration": parseUsesConfiguration(parser); break;
                case "supports-screens": parseSupportsScreens(parser); break;
                case "application": parseApplication(parser); break;
                default: break;
            }
        }
        // Implicit permissions after the explicit ones (ParsingPackageUtils.convertCompatPermissions):
        // POST_NOTIFICATIONS below targetSdk 33, and the two older ones below 4.
        int target = mApplication.targetSdkVersion;
        if (target < 33) {
            addImplicit("android.permission.POST_NOTIFICATIONS");
            if (target < 4) {
                addImplicit("android.permission.WRITE_EXTERNAL_STORAGE");
                addImplicit("android.permission.READ_PHONE_STATE");
            }
        }
        // Then the split permissions below their targetSdk, in platform.xml's order
        // (ParsingPackageUtils.convertSplitPermissions); a permission one adds can bring in a later one.
        for (Split split : splits()) {
            if (target < split.targetSdk && mRequestedPermissions.contains(split.permission)) {
                for (String permission : split.newPermissions) {
                    addImplicit(permission);
                }
            }
        }
        mApplication.flags |= screenFlags();
    }

    /** ParsingPackage.addImplicitPermission, for a permission not requested yet: requested, without flags. */
    private void addImplicit(String permission) {
        if (!mRequestedPermissions.contains(permission)) {
            mRequestedPermissions.add(permission);
            mUsesPermissionFlags.put(permission, 0);
            mImplicitPermissions.add(permission);
        }
    }

    private void parseUsesSdk(XmlResourceParser parser) {
        TypedArray sa = resources.obtainAttributes(parser, R.styleable.AndroidManifestUsesSdk);
        try {
            int min = sdk(sa, R.styleable.AndroidManifestUsesSdk_minSdkVersion, 1);
            mApplication.minSdkVersion = min;
            mApplication.targetSdkVersion = sdk(sa, R.styleable.AndroidManifestUsesSdk_targetSdkVersion, min);
        } finally {
            sa.recycle();
        }
    }

    /** An SDK level given as a number; a codename names a preview, which a release build refuses. */
    private static int sdk(TypedArray sa, int index, int fallback) {
        TypedValue v = sa.peekValue(index);
        if (v == null) {
            return fallback;
        }
        if (v.type == TypedValue.TYPE_STRING) {
            throw new IllegalStateException("SDK level " + v.string + " is a preview's");
        }
        return v.data;
    }

    private void parseUsesPermission(XmlResourceParser parser) throws IOException, XmlPullParserException {
        TypedArray sa = resources.obtainAttributes(parser, R.styleable.AndroidManifestUsesPermission);
        String name;
        int max;
        int min;
        int flags;
        List<String> required = new ArrayList<>();
        List<String> requiredNot = new ArrayList<>();
        try {
            name = sa.getNonResourceString(R.styleable.AndroidManifestUsesPermission_name);
            max = intOnly(sa, R.styleable.AndroidManifestUsesPermission_maxSdkVersion, Integer.MAX_VALUE);
            min = intOnly(sa, R.styleable.AndroidManifestUsesPermission_minSdkVersion, 0);
            flags = sa.getInt(R.styleable.AndroidManifestUsesPermission_usesPermissionFlags, 0);
            String feature = sa.getNonConfigurationString(R.styleable.AndroidManifestUsesPermission_requiredFeature, 0);
            if (feature != null) {
                required.add(feature);
            }
            feature = sa.getNonConfigurationString(R.styleable.AndroidManifestUsesPermission_requiredNotFeature, 0);
            if (feature != null) {
                requiredNot.add(feature);
            }
        } finally {
            sa.recycle();
        }
        int depth = parser.getDepth();
        int type;
        while ((type = parser.next()) != XmlPullParser.END_DOCUMENT
                && (type != XmlPullParser.END_TAG || parser.getDepth() > depth)) {
            if (type != XmlPullParser.START_TAG || parser.getDepth() != depth + 1) {
                continue;
            }
            if ("required-feature".equals(parser.getName())) {
                required.add(featureName(parser, R.styleable.AndroidManifestRequiredFeature,
                        R.styleable.AndroidManifestRequiredFeature_name));
            } else if ("required-not-feature".equals(parser.getName())) {
                requiredNot.add(featureName(parser, R.styleable.AndroidManifestRequiredNotFeature,
                        R.styleable.AndroidManifestRequiredNotFeature_name));
            }
        }
        if (name == null || SDK < min || SDK > max) {
            return;
        }
        // Requested only on a device with every required feature and none of the required-not ones, as the
        // parser's callback answers.
        boolean vulkan = vulkan();
        for (String feature : required) {
            if (!SystemFeatures.has(feature, 0, vulkan)) {
                return;
            }
        }
        for (String feature : requiredNot) {
            if (SystemFeatures.has(feature, 0, vulkan)) {
                return;
            }
        }
        // A second request is ignored when its flags agree; when they do not, the parse fails.
        Integer first = mUsesPermissionFlags.get(name);
        if (first != null) {
            if (first != flags) {
                throw new IllegalStateException("Conflicting uses-permissions flags: " + name + " in package: "
                        + packageName);
            }
            return;
        }
        mRequestedPermissions.add(name);
        mUsesPermissionFlags.put(name, flags);
    }

    /** A <required-feature>'s or <required-not-feature>'s name, which must not be empty. */
    private String featureName(XmlResourceParser parser, int[] styleable, int index) {
        TypedArray sa = resources.obtainAttributes(parser, styleable);
        try {
            String name = sa.getString(index);
            if (name == null || name.isEmpty()) {
                throw new IllegalStateException("Feature name is missing from <" + parser.getName() + "> tag.");
            }
            return name;
        } finally {
            sa.recycle();
        }
    }

    /** Whether the session has a Vulkan driver, which ro.hardware.vulkan names, as on Android (ADR-0045). */
    static boolean vulkan() {
        return !SystemProperties.get("ro.hardware.vulkan").isEmpty();
    }

    /** An int attribute only when it has an int type, as the parser reads <uses-permission>'s SDK bounds. */
    private static int intOnly(TypedArray sa, int index, int fallback) {
        TypedValue v = sa.peekValue(index);
        return v != null && v.type >= TypedValue.TYPE_FIRST_INT && v.type <= TypedValue.TYPE_LAST_INT
                ? v.data : fallback;
    }

    private void parsePermission(XmlResourceParser parser) throws IOException, XmlPullParserException {
        TypedArray sa = resources.obtainAttributes(parser, R.styleable.AndroidManifestPermission);
        boolean platform = PLATFORM.equals(packageName);
        PermissionInfo info;
        try {
            // Only the platform names a background permission.
            int background = R.styleable.AndroidManifestPermission_backgroundPermission;
            info = new PermissionInfo(platform && sa.hasValue(background) ? sa.getNonResourceString(background) : null);
            item(info, sa, R.styleable.AndroidManifestPermission_name, R.styleable.AndroidManifestPermission_label,
                    R.styleable.AndroidManifestPermission_icon, R.styleable.AndroidManifestPermission_logo,
                    R.styleable.AndroidManifestPermission_banner);
            info.descriptionRes = sa.getResourceId(R.styleable.AndroidManifestPermission_description, 0);
            info.group = sa.getNonResourceString(R.styleable.AndroidManifestPermission_permissionGroup);
            info.requestRes = sa.getResourceId(R.styleable.AndroidManifestPermission_request, 0);
            info.protectionLevel = PermissionInfo.fixProtectionLevel(sa.getInt(
                    R.styleable.AndroidManifestPermission_protectionLevel, PermissionInfo.PROTECTION_NORMAL));
            info.flags = sa.getInt(R.styleable.AndroidManifestPermission_permissionFlags, 0);
            // Only the platform's runtime permissions can be hard or soft restricted.
            if (!platform || (info.protectionLevel & PermissionInfo.PROTECTION_MASK_BASE)
                    != PermissionInfo.PROTECTION_DANGEROUS) {
                info.flags &= ~(PermissionInfo.FLAG_HARD_RESTRICTED | PermissionInfo.FLAG_SOFT_RESTRICTED);
            }
            int max = sa.getInt(R.styleable.AndroidManifestPermission_maxSdkVersion, -1);
            if (max != -1 && max < SDK) {
                return;
            }
        } finally {
            sa.recycle();
        }
        info.metaData = children(parser, null, null);
        mPermissions.add(info);
    }

    /**
     * As ParsedPermissionUtils.parsePermissionGroup: the group's description, priority and flags, with its meta-data.
     * Not read: request, requestDetail, backgroundRequest and backgroundRequestDetail, whose attributes the generated
     * R.styleable has no index for (the ids stay 0).
     */
    private void parsePermissionGroup(XmlResourceParser parser) throws IOException, XmlPullParserException {
        TypedArray sa = resources.obtainAttributes(parser, R.styleable.AndroidManifestPermissionGroup);
        PermissionGroupInfo info = new PermissionGroupInfo();
        try {
            item(info, sa, R.styleable.AndroidManifestPermissionGroup_name, R.styleable.AndroidManifestPermissionGroup_label,
                    R.styleable.AndroidManifestPermissionGroup_icon, R.styleable.AndroidManifestPermissionGroup_logo,
                    R.styleable.AndroidManifestPermissionGroup_banner);
            info.descriptionRes = sa.getResourceId(R.styleable.AndroidManifestPermissionGroup_description, 0);
            // Read for every package, as AOSP's parser fails on a priority that is no integer, but only the
            // platform's groups keep it [measured: package-answers-test on apk-runner-35].
            int priority = sa.getInt(R.styleable.AndroidManifestPermissionGroup_priority, 0);
            info.priority = PLATFORM.equals(packageName) ? priority : 0;
            info.flags = sa.getInt(R.styleable.AndroidManifestPermissionGroup_permissionGroupFlags, 0);
        } finally {
            sa.recycle();
        }
        info.metaData = children(parser, null, null);
        mPermissionGroups.add(info);
    }

    /** As ParsedInstrumentationUtils: the target package and processes and the two flags, with its meta-data. */
    private void parseInstrumentation(XmlResourceParser parser) throws IOException, XmlPullParserException {
        TypedArray sa = resources.obtainAttributes(parser, R.styleable.AndroidManifestInstrumentation);
        InstrumentationInfo info = new InstrumentationInfo();
        try {
            item(info, sa, R.styleable.AndroidManifestInstrumentation_name, R.styleable.AndroidManifestInstrumentation_label,
                    R.styleable.AndroidManifestInstrumentation_icon, R.styleable.AndroidManifestInstrumentation_logo,
                    R.styleable.AndroidManifestInstrumentation_banner);
            info.targetPackage = sa.getNonResourceString(R.styleable.AndroidManifestInstrumentation_targetPackage);
            info.targetProcesses = sa.getNonResourceString(R.styleable.AndroidManifestInstrumentation_targetProcesses);
            info.handleProfiling = sa.getBoolean(R.styleable.AndroidManifestInstrumentation_handleProfiling, false);
            info.functionalTest = sa.getBoolean(R.styleable.AndroidManifestInstrumentation_functionalTest, false);
        } finally {
            sa.recycle();
        }
        info.metaData = children(parser, null, null);
        mInstrumentations.add(info);
    }

    private void parseUsesFeature(XmlResourceParser parser) {
        TypedArray sa = resources.obtainAttributes(parser, R.styleable.AndroidManifestUsesFeature);
        try {
            FeatureInfo feature = new FeatureInfo();
            feature.name = sa.getNonResourceString(R.styleable.AndroidManifestUsesFeature_name);
            feature.version = sa.getInt(R.styleable.AndroidManifestUsesFeature_version, 0);
            if (feature.name == null) {
                feature.reqGlEsVersion = sa.getInt(R.styleable.AndroidManifestUsesFeature_glEsVersion,
                        FeatureInfo.GL_ES_VERSION_UNDEFINED);
            }
            if (sa.getBoolean(R.styleable.AndroidManifestUsesFeature_required, true)) {
                feature.flags |= FeatureInfo.FLAG_REQUIRED;
            }
            mFeatures.add(feature);
            if (feature.name == null) {
                ConfigurationInfo configuration = new ConfigurationInfo();
                configuration.reqGlEsVersion = feature.reqGlEsVersion;
                mConfigurations.add(configuration);
            }
        } finally {
            sa.recycle();
        }
    }

    private void parseUsesConfiguration(XmlResourceParser parser) {
        TypedArray sa = resources.obtainAttributes(parser, R.styleable.AndroidManifestUsesConfiguration);
        try {
            ConfigurationInfo info = new ConfigurationInfo();
            info.reqTouchScreen = sa.getInt(R.styleable.AndroidManifestUsesConfiguration_reqTouchScreen, 0);
            info.reqKeyboardType = sa.getInt(R.styleable.AndroidManifestUsesConfiguration_reqKeyboardType, 0);
            if (sa.getBoolean(R.styleable.AndroidManifestUsesConfiguration_reqHardKeyboard, false)) {
                info.reqInputFeatures |= ConfigurationInfo.INPUT_FEATURE_HARD_KEYBOARD;
            }
            info.reqNavigation = sa.getInt(R.styleable.AndroidManifestUsesConfiguration_reqNavigation, 0);
            if (sa.getBoolean(R.styleable.AndroidManifestUsesConfiguration_reqFiveWayNav, false)) {
                info.reqInputFeatures |= ConfigurationInfo.INPUT_FEATURE_FIVE_WAY_NAV;
            }
            mConfigurations.add(info);
        } finally {
            sa.recycle();
        }
    }

    private void parseSupportsScreens(XmlResourceParser parser) {
        TypedArray sa = resources.obtainAttributes(parser, R.styleable.AndroidManifestSupportsScreens);
        try {
            mApplication.requiresSmallestWidthDp = sa.getInt(
                    R.styleable.AndroidManifestSupportsScreens_requiresSmallestWidthDp, 0);
            mApplication.compatibleWidthLimitDp = sa.getInt(
                    R.styleable.AndroidManifestSupportsScreens_compatibleWidthLimitDp, 0);
            mApplication.largestWidthLimitDp = sa.getInt(
                    R.styleable.AndroidManifestSupportsScreens_largestWidthLimitDp, 0);
            // getInt of a boolean: -1 is true, 0 false; 1 stays unset.
            mSmallScreens = sa.getInt(R.styleable.AndroidManifestSupportsScreens_smallScreens, 1);
            mNormalScreens = sa.getInt(R.styleable.AndroidManifestSupportsScreens_normalScreens, 1);
            mLargeScreens = sa.getInt(R.styleable.AndroidManifestSupportsScreens_largeScreens, 1);
            mXlargeScreens = sa.getInt(R.styleable.AndroidManifestSupportsScreens_xlargeScreens, 1);
            mResizeable = sa.getInt(R.styleable.AndroidManifestSupportsScreens_resizeable, 1);
            mAnyDensity = sa.getInt(R.styleable.AndroidManifestSupportsScreens_anyDensity, 1);
        } finally {
            sa.recycle();
        }
    }

    /** The screen bits of ApplicationInfo.flags, as PackageImpl derives them: set if unset and the target allows. */
    private int screenFlags() {
        int target = mApplication.targetSdkVersion;
        return screen(mSmallScreens, target >= 4, ApplicationInfo.FLAG_SUPPORTS_SMALL_SCREENS)
                | screen(mNormalScreens, true, ApplicationInfo.FLAG_SUPPORTS_NORMAL_SCREENS)
                | screen(mLargeScreens, target >= 4, ApplicationInfo.FLAG_SUPPORTS_LARGE_SCREENS)
                | screen(mXlargeScreens, target >= 9, ApplicationInfo.FLAG_SUPPORTS_XLARGE_SCREENS)
                | screen(mResizeable, target >= 4, ApplicationInfo.FLAG_RESIZEABLE_FOR_SCREENS)
                | screen(mAnyDensity, target >= 4, ApplicationInfo.FLAG_SUPPORTS_SCREEN_DENSITIES);
    }

    private static int screen(int value, boolean byTarget, int flag) {
        return (value == 1 ? byTarget : value < 0) ? flag : 0;
    }

    // The application.

    private void parseApplication(XmlResourceParser parser) throws IOException, XmlPullParserException {
        ApplicationInfo ai = mApplication;
        int target = ai.targetSdkVersion;
        TypedArray sa = resources.obtainAttributes(parser, R.styleable.AndroidManifestApplication);
        try {
            ai.className = className(sa.getNonConfigurationString(R.styleable.AndroidManifestApplication_name, 0));
            ai.name = ai.className;
            label(ai, sa, R.styleable.AndroidManifestApplication_label);
            ai.icon = sa.getResourceId(R.styleable.AndroidManifestApplication_icon, 0);
            ai.logo = sa.getResourceId(R.styleable.AndroidManifestApplication_logo, 0);
            ai.banner = sa.getResourceId(R.styleable.AndroidManifestApplication_banner, 0);
            ai.descriptionRes = sa.getResourceId(R.styleable.AndroidManifestApplication_description, 0);
            ai.theme = sa.getResourceId(R.styleable.AndroidManifestApplication_theme, 0);
            ai.networkSecurityConfigRes = sa.getResourceId(
                    R.styleable.AndroidManifestApplication_networkSecurityConfig, 0);
            ai.manageSpaceActivityName = className(sa.getNonConfigurationString(
                    R.styleable.AndroidManifestApplication_manageSpaceActivity, Configuration.NATIVE_CONFIG_VERSION));
            ai.permission = sa.getNonConfigurationString(R.styleable.AndroidManifestApplication_permission, 0);
            ai.uiOptions = sa.getInt(R.styleable.AndroidManifestApplication_uiOptions, 0);
            ai.category = sa.getInt(R.styleable.AndroidManifestApplication_appCategory,
                    ApplicationInfo.CATEGORY_UNDEFINED);
            ai.enabled = sa.getBoolean(R.styleable.AndroidManifestApplication_enabled, true);
            ai.appComponentFactory = className(sa.getNonResourceString(
                    R.styleable.AndroidManifestApplication_appComponentFactory));
            ai.taskAffinity = target >= 8 ? taskAffinity(packageName, sa.getNonConfigurationString(
                    R.styleable.AndroidManifestApplication_taskAffinity, Configuration.NATIVE_CONFIG_VERSION))
                    : null;
            ai.processName = processName(null, sa.getNonConfigurationString(
                    R.styleable.AndroidManifestApplication_process, Configuration.NATIVE_CONFIG_VERSION));
            if (ai.processName == null) {
                ai.processName = packageName;
            }

            int flags = 0;
            boolean allowBackup = sa.getBoolean(R.styleable.AndroidManifestApplication_allowBackup, true);
            if (allowBackup) {
                flags |= ApplicationInfo.FLAG_ALLOW_BACKUP;
                ai.backupAgentName = className(sa.getNonConfigurationString(
                        R.styleable.AndroidManifestApplication_backupAgent, 0));
                if (ai.backupAgentName != null) {
                    flags |= flag(sa, R.styleable.AndroidManifestApplication_killAfterRestore, true,
                            ApplicationInfo.FLAG_KILL_AFTER_RESTORE)
                            | flag(sa, R.styleable.AndroidManifestApplication_restoreAnyVersion, false,
                                    ApplicationInfo.FLAG_RESTORE_ANY_VERSION)
                            | flag(sa, R.styleable.AndroidManifestApplication_fullBackupOnly, false,
                                    ApplicationInfo.FLAG_FULL_BACKUP_ONLY);
                }
            }
            flags |= flag(sa, R.styleable.AndroidManifestApplication_debuggable, false,
                    ApplicationInfo.FLAG_DEBUGGABLE)
                    | flag(sa, R.styleable.AndroidManifestApplication_hasCode, true, ApplicationInfo.FLAG_HAS_CODE)
                    | flag(sa, R.styleable.AndroidManifestApplication_allowTaskReparenting, false,
                            ApplicationInfo.FLAG_ALLOW_TASK_REPARENTING)
                    | flag(sa, R.styleable.AndroidManifestApplication_allowClearUserData, true,
                            ApplicationInfo.FLAG_ALLOW_CLEAR_USER_DATA)
                    | flag(sa, R.styleable.AndroidManifestApplication_testOnly, false, ApplicationInfo.FLAG_TEST_ONLY)
                    | flag(sa, R.styleable.AndroidManifestApplication_vmSafeMode, false,
                            ApplicationInfo.FLAG_VM_SAFE_MODE)
                    | flag(sa, R.styleable.AndroidManifestApplication_largeHeap, false, ApplicationInfo.FLAG_LARGE_HEAP)
                    | flag(sa, R.styleable.AndroidManifestApplication_supportsRtl, false,
                            ApplicationInfo.FLAG_SUPPORTS_RTL)
                    | flag(sa, R.styleable.AndroidManifestApplication_isGame, false, ApplicationInfo.FLAG_IS_GAME)
                    | flag(sa, R.styleable.AndroidManifestApplication_usesCleartextTraffic, target < 28,
                            ApplicationInfo.FLAG_USES_CLEARTEXT_TRAFFIC)
                    | flag(sa, R.styleable.AndroidManifestApplication_extractNativeLibs, true,
                            ApplicationInfo.FLAG_EXTRACT_NATIVE_LIBS)
                    | flag(sa, R.styleable.AndroidManifestApplication_hardwareAccelerated, target >= 14,
                            ApplicationInfo.FLAG_HARDWARE_ACCELERATED)
                    | flag(sa, R.styleable.AndroidManifestApplication_multiArch, false, ApplicationInfo.FLAG_MULTIARCH);
            // FLAG_PERSISTENT is the system's alone (ScanPackageUtils.applyPolicy clears it for an app).
            ai.flags = flags;
        } finally {
            sa.recycle();
        }

        int depth = parser.getDepth();
        int type;
        while ((type = parser.next()) != XmlPullParser.END_DOCUMENT
                && (type != XmlPullParser.END_TAG || parser.getDepth() > depth)) {
            if (type != XmlPullParser.START_TAG || parser.getDepth() != depth + 1 || skipped(parser)) {
                continue;
            }
            switch (parser.getName()) {
                case "meta-data": ai.metaData = metaData(parser, ai.metaData); break;
                case "activity": mActivities.add(parseActivity(parser, false)); break;
                case "receiver": mReceivers.add(parseActivity(parser, true)); break;
                case "activity-alias": mActivities.add(parseAlias(parser)); break;
                case "service": mServices.add(parseService(parser)); break;
                case "provider": mProviders.add(parseProvider(parser)); break;
                default: break;
            }
        }
    }

    // The components.

    private ActivityInfo parseActivity(XmlResourceParser parser, boolean receiver)
            throws IOException, XmlPullParserException {
        ApplicationInfo app = mApplication;
        ActivityInfo info = new ActivityInfo();
        boolean exportedSet;
        boolean exported;
        boolean canDisplayOnRemoteDevices;
        TypedArray sa = resources.obtainAttributes(parser, R.styleable.AndroidManifestActivity);
        try {
            mainComponent(info, sa, R.styleable.AndroidManifestActivity_name, R.styleable.AndroidManifestActivity_label,
                    R.styleable.AndroidManifestActivity_icon, R.styleable.AndroidManifestActivity_logo,
                    R.styleable.AndroidManifestActivity_banner, R.styleable.AndroidManifestActivity_description,
                    R.styleable.AndroidManifestActivity_process, R.styleable.AndroidManifestActivity_enabled,
                    R.styleable.AndroidManifestActivity_directBootAware, R.styleable.AndroidManifestActivity_splitName,
                    R.styleable.AndroidManifestActivity_attributionTags);
            info.theme = sa.getResourceId(R.styleable.AndroidManifestActivity_theme, 0);
            info.uiOptions = sa.getInt(R.styleable.AndroidManifestActivity_uiOptions, app.uiOptions);
            int flags = flag(sa, R.styleable.AndroidManifestActivity_allowTaskReparenting,
                    (app.flags & ApplicationInfo.FLAG_ALLOW_TASK_REPARENTING) != 0,
                    ActivityInfo.FLAG_ALLOW_TASK_REPARENTING)
                    | flag(sa, R.styleable.AndroidManifestActivity_alwaysRetainTaskState, false,
                            ActivityInfo.FLAG_ALWAYS_RETAIN_TASK_STATE)
                    | flag(sa, R.styleable.AndroidManifestActivity_clearTaskOnLaunch, false,
                            ActivityInfo.FLAG_CLEAR_TASK_ON_LAUNCH)
                    | flag(sa, R.styleable.AndroidManifestActivity_excludeFromRecents, false,
                            ActivityInfo.FLAG_EXCLUDE_FROM_RECENTS)
                    | flag(sa, R.styleable.AndroidManifestActivity_finishOnCloseSystemDialogs, false,
                            ActivityInfo.FLAG_FINISH_ON_CLOSE_SYSTEM_DIALOGS)
                    | flag(sa, R.styleable.AndroidManifestActivity_finishOnTaskLaunch, false,
                            ActivityInfo.FLAG_FINISH_ON_TASK_LAUNCH)
                    | flag(sa, R.styleable.AndroidManifestActivity_immersive, false, ActivityInfo.FLAG_IMMERSIVE)
                    | flag(sa, R.styleable.AndroidManifestActivity_multiprocess, false,
                            ActivityInfo.FLAG_MULTIPROCESS)
                    | flag(sa, R.styleable.AndroidManifestActivity_noHistory, false, ActivityInfo.FLAG_NO_HISTORY)
                    | flag(sa, R.styleable.AndroidManifestActivity_showForAllUsers, false,
                            ActivityInfo.FLAG_SHOW_FOR_ALL_USERS)
                    | flag(sa, R.styleable.AndroidManifestActivity_showOnLockScreen, false,
                            ActivityInfo.FLAG_SHOW_FOR_ALL_USERS)
                    | flag(sa, R.styleable.AndroidManifestActivity_stateNotNeeded, false,
                            ActivityInfo.FLAG_STATE_NOT_NEEDED)
                    | flag(sa, R.styleable.AndroidManifestActivity_systemUserOnly, false,
                            ActivityInfo.FLAG_SYSTEM_USER_ONLY);
            if (!receiver) {
                flags |= flag(sa, R.styleable.AndroidManifestActivity_hardwareAccelerated,
                        (app.flags & ApplicationInfo.FLAG_HARDWARE_ACCELERATED) != 0,
                        ActivityInfo.FLAG_HARDWARE_ACCELERATED)
                        | flag(sa, R.styleable.AndroidManifestActivity_allowEmbedded, false,
                                ActivityInfo.FLAG_ALLOW_EMBEDDED)
                        | flag(sa, R.styleable.AndroidManifestActivity_alwaysFocusable, false,
                                ActivityInfo.FLAG_ALWAYS_FOCUSABLE)
                        | flag(sa, R.styleable.AndroidManifestActivity_autoRemoveFromRecents, false,
                                ActivityInfo.FLAG_AUTO_REMOVE_FROM_RECENTS)
                        | flag(sa, R.styleable.AndroidManifestActivity_relinquishTaskIdentity, false,
                                ActivityInfo.FLAG_RELINQUISH_TASK_IDENTITY)
                        | flag(sa, R.styleable.AndroidManifestActivity_resumeWhilePausing, false,
                                ActivityInfo.FLAG_RESUME_WHILE_PAUSING)
                        | flag(sa, R.styleable.AndroidManifestActivity_showWhenLocked, false,
                                ActivityInfo.FLAG_SHOW_WHEN_LOCKED)
                        | flag(sa, R.styleable.AndroidManifestActivity_supportsPictureInPicture, false,
                                ActivityInfo.FLAG_SUPPORTS_PICTURE_IN_PICTURE)
                        | flag(sa, R.styleable.AndroidManifestActivity_turnScreenOn, false,
                                ActivityInfo.FLAG_TURN_SCREEN_ON)
                        | flag(sa, R.styleable.AndroidManifestActivity_preferMinimalPostProcessing, false,
                                ActivityInfo.FLAG_PREFER_MINIMAL_POST_PROCESSING)
                        | flag(sa, R.styleable.AndroidManifestActivity_allowUntrustedActivityEmbedding, false,
                                ActivityInfo.FLAG_ALLOW_UNTRUSTED_ACTIVITY_EMBEDDING);
                info.colorMode = sa.getInt(R.styleable.AndroidManifestActivity_colorMode,
                        ActivityInfo.COLOR_MODE_DEFAULT);
                info.documentLaunchMode = sa.getInt(R.styleable.AndroidManifestActivity_documentLaunchMode,
                        ActivityInfo.DOCUMENT_LAUNCH_NONE);
                info.launchMode = sa.getInt(R.styleable.AndroidManifestActivity_launchMode,
                        ActivityInfo.LAUNCH_MULTIPLE);
                info.lockTaskLaunchMode = sa.getInt(R.styleable.AndroidManifestActivity_lockTaskMode, 0);
                info.maxRecents = sa.getInt(R.styleable.AndroidManifestActivity_maxRecents, DEFAULT_MAX_RECENTS);
                info.persistableMode = sa.getInteger(R.styleable.AndroidManifestActivity_persistableMode,
                        ActivityInfo.PERSIST_ROOT_ONLY);
                info.requestedVrComponent = sa.getString(R.styleable.AndroidManifestActivity_enableVrMode);
                info.rotationAnimation = sa.getInt(R.styleable.AndroidManifestActivity_rotationAnimation, -1);
                info.softInputMode = sa.getInt(R.styleable.AndroidManifestActivity_windowSoftInputMode, 0);
                // MCC and MNC unless recreateOnConfigChanges names them (ParsedActivityUtils.getActivityConfigChanges).
                info.configChanges = sa.getInt(R.styleable.AndroidManifestActivity_configChanges, 0)
                        | (~sa.getInt(R.styleable.AndroidManifestActivity_recreateOnConfigChanges, 0)
                                & (ActivityInfo.CONFIG_MCC | ActivityInfo.CONFIG_MNC));
                info.screenOrientation = sa.getInt(R.styleable.AndroidManifestActivity_screenOrientation,
                        ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED);
            } else {
                info.launchMode = ActivityInfo.LAUNCH_MULTIPLE;
                info.configChanges = 0;
                info.screenOrientation = ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED;
                info.rotationAnimation = -1;
                flags |= flag(sa, R.styleable.AndroidManifestActivity_singleUser, false, ActivityInfo.FLAG_SINGLE_USER);
            }
            if (sa.getBoolean(R.styleable.AndroidManifestActivity_visibleToInstantApps, false)) {
                flags |= ActivityInfo.FLAG_VISIBLE_TO_INSTANT_APP;
            }
            info.flags = flags;
            info.taskAffinity = taskAffinity(app.taskAffinity, sa.getNonConfigurationString(
                    R.styleable.AndroidManifestActivity_taskAffinity, Configuration.NATIVE_CONFIG_VERSION));
            info.requiredDisplayCategory = sa.getNonConfigurationString(
                    R.styleable.AndroidManifestActivity_requiredDisplayCategory, 0);
            String parent = sa.getNonConfigurationString(R.styleable.AndroidManifestActivity_parentActivityName,
                    Configuration.NATIVE_CONFIG_VERSION);
            if (parent != null) {
                info.parentActivityName = className(parent);
            }
            String permission = sa.getNonConfigurationString(R.styleable.AndroidManifestActivity_permission, 0);
            info.permission = permission != null ? permission : app.permission;
            exportedSet = sa.hasValue(R.styleable.AndroidManifestActivity_exported);
            exported = sa.getBoolean(R.styleable.AndroidManifestActivity_exported, false);
            canDisplayOnRemoteDevices = sa.getBoolean(R.styleable.AndroidManifestActivity_canDisplayOnRemoteDevices,
                    true);
        } finally {
            sa.recycle();
        }
        int[] filters = {0, 0, 0};
        List<IntentFilter> own = new ArrayList<>();
        info.metaData = children(parser, filters, own);
        // An intent filter without an action is dropped, so it does not count.
        info.exported = exportedSet ? exported : filters[0] > 0;
        keepFilters(info, own, !receiver);
        if (!receiver && filters[2] > 0) {
            mLaunchers.add(info);
        }
        Bundle meta = info.metaData;
        if (!receiver && meta != null && "singleInstancePerTask".equals(meta.getString(META_LAUNCH_MODE))) {
            info.launchMode = ActivityInfo.LAUNCH_SINGLE_INSTANCE_PER_TASK;
        }
        if (canDisplayOnRemoteDevices && (meta == null || meta.getBoolean(META_REMOTE_DEVICES, true))) {
            info.flags |= ActivityInfo.FLAG_CAN_DISPLAY_ON_REMOTE_DEVICES;
        }
        return info;
    }

    /** An alias is a copy of its target, which comes before it, with its own name and a few of its own attributes. */
    private ActivityInfo parseAlias(XmlResourceParser parser) throws IOException, XmlPullParserException {
        ActivityInfo info;
        boolean exportedSet;
        boolean exported;
        TypedArray sa = resources.obtainAttributes(parser, R.styleable.AndroidManifestActivityAlias);
        try {
            String target = className(sa.getNonConfigurationString(
                    R.styleable.AndroidManifestActivityAlias_targetActivity, Configuration.NATIVE_CONFIG_VERSION));
            ActivityInfo targetInfo = null;
            for (ActivityInfo activity : mActivities) {
                if (activity.targetActivity == null && activity.name.equals(target)) {
                    targetInfo = activity;
                }
            }
            if (targetInfo == null) {
                throw new IllegalStateException("<activity-alias> target " + target + " is not an earlier activity");
            }
            info = new ActivityInfo(targetInfo);
            info.targetActivity = target;
            info.metaData = null;
            info.permission = null;
            info.persistableMode = ActivityInfo.PERSIST_ROOT_ONLY;
            info.rotationAnimation = -1;
            info.colorMode = ActivityInfo.COLOR_MODE_DEFAULT;
            info.splitName = null;
            info.attributionTags = new String[0];
            info.name = className(sa.getNonConfigurationString(R.styleable.AndroidManifestActivityAlias_name, 0));
            int icon = sa.getResourceId(R.styleable.AndroidManifestActivityAlias_icon, 0);
            if (icon != 0) {
                info.icon = icon;
                info.nonLocalizedLabel = null;
            }
            int logo = sa.getResourceId(R.styleable.AndroidManifestActivityAlias_logo, 0);
            if (logo != 0) {
                info.logo = logo;
            }
            int banner = sa.getResourceId(R.styleable.AndroidManifestActivityAlias_banner, 0);
            if (banner != 0) {
                info.banner = banner;
            }
            info.descriptionRes = sa.getResourceId(R.styleable.AndroidManifestActivityAlias_description, 0);
            if (sa.peekValue(R.styleable.AndroidManifestActivityAlias_label) != null) {
                info.labelRes = 0;
                info.nonLocalizedLabel = null;
                label(info, sa, R.styleable.AndroidManifestActivityAlias_label);
            }
            info.enabled = sa.getBoolean(R.styleable.AndroidManifestActivityAlias_enabled, true);
            String parent = sa.getNonConfigurationString(R.styleable.AndroidManifestActivityAlias_parentActivityName,
                    Configuration.NATIVE_CONFIG_VERSION);
            if (parent != null) {
                info.parentActivityName = className(parent);
            }
            info.permission = sa.getNonConfigurationString(R.styleable.AndroidManifestActivityAlias_permission, 0);
            exportedSet = sa.hasValue(R.styleable.AndroidManifestActivityAlias_exported);
            exported = sa.getBoolean(R.styleable.AndroidManifestActivityAlias_exported, false);
        } finally {
            sa.recycle();
        }
        int[] filters = {0, 0, 0};
        List<IntentFilter> own = new ArrayList<>();
        info.metaData = children(parser, filters, own);
        info.exported = exportedSet ? exported : filters[0] > 0;
        keepFilters(info, own, true);
        if (filters[2] > 0) {
            mLaunchers.add(info);
        }
        return info;
    }

    private ServiceInfo parseService(XmlResourceParser parser) throws IOException, XmlPullParserException {
        ServiceInfo info = new ServiceInfo();
        boolean exportedSet;
        boolean exported;
        TypedArray sa = resources.obtainAttributes(parser, R.styleable.AndroidManifestService);
        try {
            mainComponent(info, sa, R.styleable.AndroidManifestService_name, R.styleable.AndroidManifestService_label,
                    R.styleable.AndroidManifestService_icon, R.styleable.AndroidManifestService_logo,
                    R.styleable.AndroidManifestService_banner, R.styleable.AndroidManifestService_description,
                    R.styleable.AndroidManifestService_process, R.styleable.AndroidManifestService_enabled,
                    R.styleable.AndroidManifestService_directBootAware, R.styleable.AndroidManifestService_splitName,
                    R.styleable.AndroidManifestService_attributionTags);
            String permission = sa.getNonConfigurationString(R.styleable.AndroidManifestService_permission, 0);
            info.permission = permission != null ? permission : mApplication.permission;
            info.mForegroundServiceType = sa.getInt(R.styleable.AndroidManifestService_foregroundServiceType,
                    ServiceInfo.FOREGROUND_SERVICE_TYPE_NONE);
            info.flags = flag(sa, R.styleable.AndroidManifestService_stopWithTask, false, ServiceInfo.FLAG_STOP_WITH_TASK)
                    | flag(sa, R.styleable.AndroidManifestService_isolatedProcess, false,
                            ServiceInfo.FLAG_ISOLATED_PROCESS)
                    | flag(sa, R.styleable.AndroidManifestService_externalService, false,
                            ServiceInfo.FLAG_EXTERNAL_SERVICE)
                    | flag(sa, R.styleable.AndroidManifestService_useAppZygote, false,
                            ServiceInfo.FLAG_USE_APP_ZYGOTE)
                    | flag(sa, R.styleable.AndroidManifestService_allowSharedIsolatedProcess, false,
                            ServiceInfo.FLAG_ALLOW_SHARED_ISOLATED_PROCESS)
                    | flag(sa, R.styleable.AndroidManifestService_singleUser, false, ServiceInfo.FLAG_SINGLE_USER)
                    | flag(sa, R.styleable.AndroidManifestService_visibleToInstantApps, false,
                            ServiceInfo.FLAG_VISIBLE_TO_INSTANT_APP);
            exportedSet = sa.hasValue(R.styleable.AndroidManifestService_exported);
            exported = sa.getBoolean(R.styleable.AndroidManifestService_exported, false);
        } finally {
            sa.recycle();
        }
        int[] filters = {0, 0, 0};
        List<IntentFilter> own = new ArrayList<>();
        info.metaData = children(parser, filters, own);
        // A service's intent filter counts even without an action.
        info.exported = exportedSet ? exported : filters[1] > 0;
        mFilters.put(info, own);
        return info;
    }

    private ProviderInfo parseProvider(XmlResourceParser parser) throws IOException, XmlPullParserException {
        ProviderInfo info = new ProviderInfo();
        TypedArray sa = resources.obtainAttributes(parser, R.styleable.AndroidManifestProvider);
        try {
            mainComponent(info, sa, R.styleable.AndroidManifestProvider_name, R.styleable.AndroidManifestProvider_label,
                    R.styleable.AndroidManifestProvider_icon, R.styleable.AndroidManifestProvider_logo,
                    R.styleable.AndroidManifestProvider_banner, R.styleable.AndroidManifestProvider_description,
                    R.styleable.AndroidManifestProvider_process, R.styleable.AndroidManifestProvider_enabled,
                    R.styleable.AndroidManifestProvider_directBootAware, R.styleable.AndroidManifestProvider_splitName,
                    R.styleable.AndroidManifestProvider_attributionTags);
            info.authority = sa.getNonConfigurationString(R.styleable.AndroidManifestProvider_authorities, 0);
            if (info.authority == null || info.authority.isEmpty()) {
                throw new IllegalStateException("<provider> " + info.name + " has no authorities");
            }
            info.isSyncable = sa.getBoolean(R.styleable.AndroidManifestProvider_syncable, false);
            info.exported = sa.getBoolean(R.styleable.AndroidManifestProvider_exported,
                    mApplication.targetSdkVersion < 17);
            String permission = sa.getNonConfigurationString(R.styleable.AndroidManifestProvider_permission, 0);
            String read = sa.getNonConfigurationString(R.styleable.AndroidManifestProvider_readPermission, 0);
            String write = sa.getNonConfigurationString(R.styleable.AndroidManifestProvider_writePermission, 0);
            String fallback = permission != null ? permission : mApplication.permission;
            info.readPermission = read != null ? read : fallback;
            info.writePermission = write != null ? write : fallback;
            info.grantUriPermissions = sa.getBoolean(R.styleable.AndroidManifestProvider_grantUriPermissions, false);
            info.forceUriPermissions = sa.getBoolean(R.styleable.AndroidManifestProvider_forceUriPermissions, false);
            info.multiprocess = sa.getBoolean(R.styleable.AndroidManifestProvider_multiprocess, false);
            info.initOrder = sa.getInt(R.styleable.AndroidManifestProvider_initOrder, 0);
            info.flags = flag(sa, R.styleable.AndroidManifestProvider_singleUser, false, ProviderInfo.FLAG_SINGLE_USER)
                    | flag(sa, R.styleable.AndroidManifestProvider_visibleToInstantApps, false,
                            ProviderInfo.FLAG_VISIBLE_TO_INSTANT_APP);
        } finally {
            sa.recycle();
        }
        List<IntentFilter> own = new ArrayList<>();
        info.metaData = children(parser, new int[3], own);
        mFilters.put(info, own);
        info.pathPermissions = new PathPermission[0];
        return info;
    }

    /** What every main component reads (ParsedComponentUtils and ParsedMainComponentUtils). */
    private void mainComponent(ComponentInfo info, TypedArray sa, int name, int label, int icon, int logo, int banner,
            int description, int process, int enabled, int directBootAware, int splitName, int attributionTags) {
        item(info, sa, name, label, icon, logo, banner);
        info.descriptionRes = sa.getResourceId(description, 0);
        info.processName = processName(mApplication.processName,
                sa.getNonConfigurationString(process, Configuration.NATIVE_CONFIG_VERSION));
        info.enabled = sa.getBoolean(enabled, true);
        info.directBootAware = sa.getBoolean(directBootAware, false);
        info.splitName = sa.getNonConfigurationString(splitName, 0);
        String tags = sa.getNonConfigurationString(attributionTags, 0);
        // getAttributionTags() of a component without them is EmptyArray.STRING.
        info.attributionTags = tags != null ? tags.split("\\|") : new String[0];
    }

    /** Name, icon, logo, banner and label, as ParsedComponentUtils.parseComponent reads them. */
    private void item(PackageItemInfo info, TypedArray sa, int name, int label, int icon, int logo, int banner) {
        info.name = className(sa.getNonConfigurationString(name, 0));
        if (info.name == null) {
            throw new IllegalStateException("a component of " + packageName + " has no name");
        }
        info.packageName = packageName;
        int iconRes = sa.getResourceId(icon, 0);
        if (iconRes != 0) {
            info.icon = iconRes;
            info.nonLocalizedLabel = null;
        }
        info.logo = sa.getResourceId(logo, 0);
        info.banner = sa.getResourceId(banner, 0);
        label(info, sa, label);
    }

    private static void label(PackageItemInfo info, TypedArray sa, int index) {
        TypedValue v = sa.peekValue(index);
        if (v != null) {
            info.labelRes = v.resourceId;
            if (v.resourceId == 0) {
                info.nonLocalizedLabel = v.coerceToString();
            }
        }
    }

    /**
     * A component's children: its <meta-data> as a Bundle, null if it has none, and in filters[0] the number of
     * <intent-filter>s with an action (an activity's, receiver's or alias's count), in filters[1] of all of them, in
     * filters[2] of those a home screen's launch intent (MAIN, LAUNCHER, no data) matches; the filters themselves into
     * `out` unless it is null.
     */
    private Bundle children(XmlResourceParser parser, int[] filters, List<IntentFilter> out)
            throws IOException, XmlPullParserException {
        Bundle metaData = null;
        int depth = parser.getDepth();
        int type;
        while ((type = parser.next()) != XmlPullParser.END_DOCUMENT
                && (type != XmlPullParser.END_TAG || parser.getDepth() > depth)) {
            if (type != XmlPullParser.START_TAG || parser.getDepth() != depth + 1 || skipped(parser)) {
                continue;
            }
            if ("meta-data".equals(parser.getName())) {
                metaData = metaData(parser, metaData);
            } else if ("intent-filter".equals(parser.getName()) && filters != null) {
                boolean[] data = {false};
                IntentFilter filter = intentFilter(parser, data);
                if (filter.countActions() > 0) {
                    filters[0]++;
                }
                // A home screen's launch intent matches MAIN with LAUNCHER and no <data>.
                if (filter.hasAction(Intent.ACTION_MAIN) && filter.hasCategory(Intent.CATEGORY_LAUNCHER) && !data[0]) {
                    filters[2]++;
                }
                filters[1]++;
                if (out != null) {
                    out.add(filter);
                }
            }
        }
        return metaData;
    }

    /**
     * One <intent-filter> as ParsedIntentInfoUtils.parseIntentInfo and parseData read it at android-15.0.0_r1: its
     * priority and order, then its actions and categories by name and its <data>'s MIME types, MIME groups, schemes,
     * scheme-specific parts, authorities and paths, each pattern kind as AOSP adds it (globs allowed, as for every
     * component of an app). data[0] tells whether it has a <data> element. A malformed MIME type, which fails the
     * import on Android, throws.
     */
    private IntentFilter intentFilter(XmlResourceParser parser, boolean[] data)
            throws IOException, XmlPullParserException {
        IntentFilter filter = new IntentFilter();
        TypedArray sa = resources.obtainAttributes(parser, R.styleable.AndroidManifestIntentFilter);
        try {
            filter.setPriority(sa.getInt(R.styleable.AndroidManifestIntentFilter_priority, 0));
            filter.setOrder(sa.getInt(R.styleable.AndroidManifestIntentFilter_order, 0));
            filter.setAutoVerify(sa.getBoolean(R.styleable.AndroidManifestIntentFilter_autoVerify, false));
        } finally {
            sa.recycle();
        }
        int depth = parser.getDepth();
        int type;
        while ((type = parser.next()) != XmlPullParser.END_DOCUMENT
                && (type != XmlPullParser.END_TAG || parser.getDepth() > depth)) {
            if (type != XmlPullParser.START_TAG || skipped(parser)) {
                continue;
            }
            String name = parser.getAttributeValue(ANDROID_NAMESPACE, "name");
            switch (parser.getName()) {
                case "action":
                    if (name != null) {
                        filter.addAction(name);
                    }
                    break;
                case "category":
                    if (name != null) {
                        filter.addCategory(name);
                    }
                    break;
                case "data":
                    data[0] = true;
                    data(parser, filter);
                    break;
                default:
                    break;
            }
        }
        return filter;
    }

    private void data(XmlResourceParser parser, IntentFilter filter) {
        TypedArray sa = resources.obtainAttributes(parser, R.styleable.AndroidManifestData);
        try {
            String str = sa.getNonConfigurationString(R.styleable.AndroidManifestData_mimeType, 0);
            if (str != null) {
                try {
                    filter.addDataType(str);
                } catch (IntentFilter.MalformedMimeTypeException e) {
                    throw new IllegalStateException(e.toString());
                }
            }
            str = sa.getNonConfigurationString(R.styleable.AndroidManifestData_mimeGroup, 0);
            if (str != null) {
                filter.addMimeGroup(str);
            }
            str = sa.getNonConfigurationString(R.styleable.AndroidManifestData_scheme, 0);
            if (str != null) {
                filter.addDataScheme(str);
            }
            schemeSpecificPart(sa, filter, R.styleable.AndroidManifestData_ssp, PatternMatcher.PATTERN_LITERAL);
            schemeSpecificPart(sa, filter, R.styleable.AndroidManifestData_sspPrefix, PatternMatcher.PATTERN_PREFIX);
            schemeSpecificPart(sa, filter, R.styleable.AndroidManifestData_sspPattern,
                    PatternMatcher.PATTERN_SIMPLE_GLOB);
            schemeSpecificPart(sa, filter, R.styleable.AndroidManifestData_sspAdvancedPattern,
                    PatternMatcher.PATTERN_ADVANCED_GLOB);
            schemeSpecificPart(sa, filter, R.styleable.AndroidManifestData_sspSuffix, PatternMatcher.PATTERN_SUFFIX);
            String host = sa.getNonConfigurationString(R.styleable.AndroidManifestData_host, 0);
            String port = sa.getNonConfigurationString(R.styleable.AndroidManifestData_port, 0);
            if (host != null) {
                filter.addDataAuthority(host, port);
            }
            path(sa, filter, R.styleable.AndroidManifestData_path, PatternMatcher.PATTERN_LITERAL);
            path(sa, filter, R.styleable.AndroidManifestData_pathPrefix, PatternMatcher.PATTERN_PREFIX);
            path(sa, filter, R.styleable.AndroidManifestData_pathPattern, PatternMatcher.PATTERN_SIMPLE_GLOB);
            path(sa, filter, R.styleable.AndroidManifestData_pathAdvancedPattern, PatternMatcher.PATTERN_ADVANCED_GLOB);
            path(sa, filter, R.styleable.AndroidManifestData_pathSuffix, PatternMatcher.PATTERN_SUFFIX);
        } finally {
            sa.recycle();
        }
    }

    private static void schemeSpecificPart(TypedArray sa, IntentFilter filter, int index, int kind) {
        String str = sa.getNonConfigurationString(index, 0);
        if (str != null) {
            filter.addDataSchemeSpecificPart(str, kind);
        }
    }

    private static void path(TypedArray sa, IntentFilter filter, int index, int kind) {
        String str = sa.getNonConfigurationString(index, 0);
        if (str != null) {
            filter.addDataPath(str, kind);
        }
    }

    /**
     * An activity's, alias's or receiver's filters: one without an action is dropped, as the package parser drops it.
     * An activity's priority above 0 becomes 0, as ComponentResolver.adjustPriority sets it for an app that is not a
     * privileged system app [measured: intent-query-test on apk-runner-35]; a receiver keeps its own.
     */
    private void keepFilters(ActivityInfo info, List<IntentFilter> filters, boolean activity) {
        filters.removeIf(filter -> filter.countActions() == 0);
        if (activity) {
            for (IntentFilter filter : filters) {
                if (filter.getPriority() > 0) {
                    filter.setPriority(0);
                }
            }
        }
        mFilters.put(info, filters);
    }

    /** One <meta-data> into the Bundle, as ParsingPackageUtils.parseMetaData and Property.toBundle put it. */
    private Bundle metaData(XmlResourceParser parser, Bundle into) {
        TypedArray sa = resources.obtainAttributes(parser, R.styleable.AndroidManifestMetaData);
        try {
            String name = sa.getNonConfigurationString(R.styleable.AndroidManifestMetaData_name, 0);
            if (name == null) {
                throw new IllegalStateException("<meta-data> has no android:name");
            }
            Bundle bundle = into != null ? into : new Bundle();
            TypedValue v = sa.peekValue(R.styleable.AndroidManifestMetaData_resource);
            if (v != null && v.resourceId != 0) {
                bundle.putInt(name, v.resourceId);
                return bundle;
            }
            v = sa.peekValue(R.styleable.AndroidManifestMetaData_value);
            if (v == null) {
                throw new IllegalStateException("<meta-data> " + name + " has no value or resource");
            }
            if (v.type == TypedValue.TYPE_STRING) {
                CharSequence cs = v.coerceToString();
                bundle.putString(name, cs != null ? cs.toString() : null);
            } else if (v.type == TypedValue.TYPE_INT_BOOLEAN) {
                bundle.putBoolean(name, v.data != 0);
            } else if (v.type >= TypedValue.TYPE_FIRST_INT && v.type <= TypedValue.TYPE_LAST_INT) {
                bundle.putInt(name, v.data);
            } else if (v.type == TypedValue.TYPE_FLOAT) {
                bundle.putFloat(name, v.getFloat());
            } else {
                // A dimension or fraction: Android logs it and leaves it out.
                return into;
            }
            return bundle;
        } finally {
            sa.recycle();
        }
    }

    // Names, as ParsingUtils and ComponentParseUtils build them.

    private String className(String name) {
        if (name == null || name.isEmpty()) {
            return null;
        }
        if (name.charAt(0) == '.') {
            return packageName + name;
        }
        return name.indexOf('.') < 0 ? packageName + "." + name : name;
    }

    private String processName(String fallback, String name) {
        return name == null || name.isEmpty() ? fallback : compoundName(name);
    }

    private String taskAffinity(String fallback, String name) {
        if (name == null) {
            return fallback;
        }
        return name.isEmpty() ? null : compoundName(name);
    }

    private String compoundName(String name) {
        return name.charAt(0) == ':' ? packageName + name : name;
    }

    private static int flag(TypedArray sa, int index, boolean fallback, int flag) {
        return sa.getBoolean(index, fallback) ? flag : 0;
    }

    // The install.

    /**
     * What the installer and system_server add: decision 6's folders, decision 3's uid, the install time, and the
     * framework's overlays (ADR-0043 f)).
     */
    private void install(String apk, String libraryDir, String dataDir) {
        ApplicationInfo ai = mApplication;
        ai.sourceDir = apk;
        ai.publicSourceDir = apk;
        ai.overlayPaths = FRAMEWORK_OVERLAYS.clone();
        ai.nativeLibraryDir = libraryDir;
        ai.nativeLibraryRootDir = new File(libraryDir).getParent();
        ai.primaryCpuAbi = "arm64-v8a";
        ai.dataDir = dataDir;
        ai.credentialProtectedDataDir = dataDir;
        ai.deviceProtectedDataDir = "/data/user_de/0/" + packageName;
        ai.storageUuid = UUID_DEFAULT;
        ai.uid = Process.myUid();
        ai.flags |= ApplicationInfo.FLAG_INSTALLED;
        for (PermissionInfo permission : mPermissions) {
            permission.flags |= PermissionInfo.FLAG_INSTALLED;
        }
        ai.setVersionCode(mPackage.getLongVersionCode());
        long installed = new File(apk).lastModified();
        mPackage.firstInstallTime = installed;
        mPackage.lastUpdateTime = installed;
    }

    // The answers, as PackageInfoCommonUtils makes them.

    PackageInfo packageInfo(long flags) {
        PackageInfo info = new PackageInfo();
        info.packageName = mPackage.packageName;
        info.splitNames = mPackage.splitNames.clone();
        info.versionCode = mPackage.versionCode;
        info.versionCodeMajor = mPackage.versionCodeMajor;
        info.baseRevisionCode = mPackage.baseRevisionCode;
        info.splitRevisionCodes = mPackage.splitRevisionCodes.clone();
        info.versionName = mPackage.versionName;
        info.sharedUserId = mPackage.sharedUserId;
        info.sharedUserLabel = mPackage.sharedUserLabel;
        info.installLocation = mPackage.installLocation;
        info.firstInstallTime = mPackage.firstInstallTime;
        info.lastUpdateTime = mPackage.lastUpdateTime;
        info.compileSdkVersion = mApplication.compileSdkVersion;
        info.compileSdkVersionCodename = mApplication.compileSdkVersionCodename;
        ApplicationInfo app = applicationInfo(flags);
        info.applicationInfo = app;
        if ((flags & PackageManager.GET_CONFIGURATIONS) != 0) {
            info.configPreferences = mConfigurations.isEmpty() ? null
                    : mConfigurations.toArray(new ConfigurationInfo[0]);
            info.reqFeatures = mFeatures.isEmpty() ? null : mFeatures.toArray(new FeatureInfo[0]);
        }
        if ((flags & PackageManager.GET_INSTRUMENTATION) != 0 && !mInstrumentations.isEmpty()) {
            info.instrumentation = new InstrumentationInfo[mInstrumentations.size()];
            for (int i = 0; i < info.instrumentation.length; i++) {
                info.instrumentation[i] = instrumentationInfo(mInstrumentations.get(i), flags, app);
            }
        }
        if ((flags & PackageManager.GET_PERMISSIONS) != 0) {
            if (!mPermissions.isEmpty()) {
                info.permissions = new PermissionInfo[mPermissions.size()];
                for (int i = 0; i < info.permissions.length; i++) {
                    info.permissions[i] = permissionInfo(mPermissions.get(i), flags);
                }
            }
            if (!mRequestedPermissions.isEmpty()) {
                info.requestedPermissions = mRequestedPermissions.toArray(new String[0]);
                info.requestedPermissionsFlags = new int[info.requestedPermissions.length];
                for (int i = 0; i < info.requestedPermissionsFlags.length; i++) {
                    String name = info.requestedPermissions[i];
                    int requested = PackageInfo.REQUESTED_PERMISSION_REQUIRED;
                    if ((mUsesPermissionFlags.get(name) & PackageInfo.REQUESTED_PERMISSION_NEVER_FOR_LOCATION) != 0) {
                        requested |= PackageInfo.REQUESTED_PERMISSION_NEVER_FOR_LOCATION;
                    }
                    if (mImplicitPermissions.contains(name)) {
                        requested |= PackageInfo.REQUESTED_PERMISSION_IMPLICIT;
                    }
                    // What system_server adds from the permission's state.
                    if (granted(name)) {
                        requested |= PackageInfo.REQUESTED_PERMISSION_GRANTED;
                    }
                    info.requestedPermissionsFlags[i] = requested;
                }
            }
        }
        if ((flags & PackageManager.GET_ACTIVITIES) != 0) {
            info.activities = activities(mActivities, flags, app);
        }
        if ((flags & PackageManager.GET_RECEIVERS) != 0) {
            info.receivers = activities(mReceivers, flags, app);
        }
        if ((flags & PackageManager.GET_SERVICES) != 0) {
            List<ServiceInfo> services = new ArrayList<>();
            for (ServiceInfo service : mServices) {
                if (matches(service, flags)) {
                    services.add(serviceInfo(service, flags, app));
                }
            }
            info.services = services.isEmpty() ? null : services.toArray(new ServiceInfo[0]);
        }
        if ((flags & PackageManager.GET_PROVIDERS) != 0) {
            List<ProviderInfo> providers = new ArrayList<>();
            for (ProviderInfo provider : mProviders) {
                if (matches(provider, flags)) {
                    providers.add(providerInfo(provider, flags, app));
                }
            }
            info.providers = providers.isEmpty() ? null : providers.toArray(new ProviderInfo[0]);
        }
        // As PackageInfoCommonUtils gives them (PackageSigning).
        if ((flags & PackageManager.GET_SIGNATURES) != 0) {
            info.signatures = PackageSigning.signatures(mSigningDetails);
            noX509();
        }
        if ((flags & PackageManager.GET_SIGNING_CERTIFICATES) != 0) {
            info.signingInfo = PackageSigning.signingInfo(mSigningDetails);
            noX509();
        }
        return info;
    }

    /**
     * The boot jars hold no X.509 provider (Conscrypt or BouncyCastle), so Signature.getPublicKey,
     * SigningInfo.getPublicKeys and an app's own CertificateFactory have nothing to parse with: logged once, an open
     * item of ADR-0039. The certificates' bytes, which most checks hash, are Android's.
     */
    private static synchronized void noX509() {
        if (!sX509Logged) {
            sX509Logged = true;
            Slog.w("PackageManager", "Signatures without public keys: the boot jars have no X.509 provider");
        }
    }

    ApplicationInfo applicationInfo(long flags) {
        ApplicationInfo info = new ApplicationInfo(mApplication);
        if ((flags & PackageManager.GET_META_DATA) == 0) {
            info.metaData = null;
        }
        if ((flags & PackageManager.GET_SHARED_LIBRARY_FILES) == 0) {
            info.sharedLibraryFiles = null;
        }
        return info;
    }

    /**
     * The activity a home screen starts (ADR-0039 step c2c3): the first enabled activity or alias of the manifest
     * whose filter the launch intent (MAIN, LAUNCHER) matches, as the launcher's queryIntentActivities lists them in
     * manifest order; null for an app without one.
     */
    ActivityInfo launcherActivity(long flags) {
        for (ActivityInfo activity : mLaunchers) {
            if (matches(activity, flags)) {
                return activityInfo(activity, flags, applicationInfo(flags));
            }
        }
        return null;
    }

    /** The activity, alias or receiver of that name, or null. */
    ActivityInfo activityInfo(ComponentName component, long flags, boolean receiver) {
        List<ActivityInfo> list = receiver ? mReceivers : mActivities;
        for (int i = list.size() - 1; i >= 0; i--) {
            if (named(list.get(i), component) && matches(list.get(i), flags)) {
                return activityInfo(list.get(i), flags, applicationInfo(flags));
            }
        }
        return null;
    }

    ServiceInfo serviceInfo(ComponentName component, long flags) {
        for (int i = mServices.size() - 1; i >= 0; i--) {
            if (named(mServices.get(i), component) && matches(mServices.get(i), flags)) {
                return serviceInfo(mServices.get(i), flags, applicationInfo(flags));
            }
        }
        return null;
    }

    /**
     * The providers Android installs as the process `processName` starts (step c2c1), as ActivityManagerService's
     * generateApplicationProvidersLocked asks the package manager for them: the enabled ones of that process, with
     * STOCK_PM_FLAGS, GET_URI_PERMISSION_PATTERNS and MATCH_DIRECT_BOOT_AUTO, by initOrder, highest first, and in the
     * manifest's order among equals, as the package manager's sort keeps it.
     */
    List<ProviderInfo> processProviders(String processName) {
        long flags = PackageManager.GET_SHARED_LIBRARY_FILES | PackageManager.GET_URI_PERMISSION_PATTERNS
                | PackageManager.MATCH_DIRECT_BOOT_AUTO;
        ApplicationInfo app = applicationInfo(flags);
        List<ProviderInfo> providers = new ArrayList<>();
        for (ProviderInfo provider : mProviders) {
            if (matches(provider, flags) && processName.equals(provider.processName)) {
                providers.add(providerInfo(provider, flags, app));
            }
        }
        providers.sort((a, b) -> Integer.compare(b.initOrder, a.initOrder));
        return providers;
    }

    /**
     * The provider one of whose authorities is `authority`, as the package manager's resolveContentProvider finds it
     * (step c2c1.1), of any process; a disabled one only with MATCH_DISABLED_COMPONENTS. Null for none.
     */
    ProviderInfo providerForAuthority(String authority, long flags) {
        for (ProviderInfo provider : mProviders) {
            if (provider.authority != null && matches(provider, flags)
                    && java.util.Arrays.asList(provider.authority.split(";")).contains(authority)) {
                return providerInfo(provider, flags, applicationInfo(flags));
            }
        }
        return null;
    }

    ProviderInfo providerInfo(ComponentName component, long flags) {
        for (int i = mProviders.size() - 1; i >= 0; i--) {
            if (named(mProviders.get(i), component) && matches(mProviders.get(i), flags)) {
                return providerInfo(mProviders.get(i), flags, applicationInfo(flags));
            }
        }
        return null;
    }

    static final int QUERY_ACTIVITIES = 0;
    static final int QUERY_RECEIVERS = 1;
    static final int QUERY_SERVICES = 2;
    static final int QUERY_PROVIDERS = 3;

    /**
     * The app's components of one kind that `intent` resolves to (step SV1), as the package manager's
     * queryIntentActivities, queryBroadcastReceivers, queryIntentServices and queryIntentContentProviders answer for
     * one package [measured: intent-query-test on apk-runner-35]:
     * - an intent for another package resolves to nothing, as no other package is installed;
     * - an explicit intent gives its component, if it is the app's and enabled, with no filter, match 0 and priority 0;
     * - else every enabled component with a filter that matches (IntentFilter.match over the action, `resolvedType`,
     *   the data and the categories), the first such filter in manifest order for each; an activity's only with the
     *   DEFAULT category under MATCH_DEFAULT_ONLY. Each result carries the filter's priority and the match, the filter
     *   itself only with GET_RESOLVED_FILTER; isDefault stays false, as Android 15 reports it;
     * - sorted by the filters' priority, highest first and in manifest order among equals, as IntentResolver sorts.
     * `state` gives a component's state that setComponentEnabledSetting left (COMPONENT_ENABLED_STATE_DEFAULT for
     * none): enabled overrides the manifest's enabled="false", and disabled hides it, unless MATCH_DISABLED_COMPONENTS.
     */
    List<ResolveInfo> query(int kind, Intent intent, String resolvedType, long flags,
            ToIntFunction<ComponentName> state) {
        List<ResolveInfo> results = new ArrayList<>();
        String target = intent.getComponent() != null ? intent.getComponent().getPackageName() : intent.getPackage();
        if (target != null && !target.equals(packageName)) {
            return results;
        }
        ApplicationInfo app = applicationInfo(flags);
        boolean defaultOnly = kind == QUERY_ACTIVITIES && (flags & PackageManager.MATCH_DEFAULT_ONLY) != 0;
        for (ComponentInfo component : components(kind)) {
            ComponentName name = new ComponentName(packageName, component.name);
            if (!enabled(component, name, flags, state)) {
                continue;
            }
            if (intent.getComponent() != null) {
                if (intent.getComponent().getClassName().equals(component.name)) {
                    results.add(result(kind, component, flags, app));
                }
                continue;
            }
            for (IntentFilter filter : mFilters.getOrDefault(component, Collections.emptyList())) {
                if (defaultOnly && !filter.hasCategory(Intent.CATEGORY_DEFAULT)) {
                    continue;
                }
                int match = filter.match(intent.getAction(), resolvedType, intent.getScheme(), intent.getData(),
                        intent.getCategories(), "PackageManager");
                if (match >= 0) {
                    ResolveInfo info = result(kind, component, flags, app);
                    info.priority = filter.getPriority();
                    info.match = match;
                    if ((flags & PackageManager.GET_RESOLVED_FILTER) != 0) {
                        info.filter = new IntentFilter(filter);
                    }
                    results.add(info);
                    break;
                }
            }
        }
        results.sort((a, b) -> Integer.compare(b.priority, a.priority));
        return results;
    }

    private List<? extends ComponentInfo> components(int kind) {
        switch (kind) {
            case QUERY_ACTIVITIES: return mActivities;
            case QUERY_RECEIVERS: return mReceivers;
            case QUERY_SERVICES: return mServices;
            default: return mProviders;
        }
    }

    private static boolean enabled(ComponentInfo component, ComponentName name, long flags,
            ToIntFunction<ComponentName> state) {
        if ((flags & PackageManager.MATCH_DISABLED_COMPONENTS) != 0) {
            return true;
        }
        switch (state.applyAsInt(name)) {
            case PackageManager.COMPONENT_ENABLED_STATE_ENABLED:
                return true;
            case PackageManager.COMPONENT_ENABLED_STATE_DEFAULT:
                return component.enabled;
            default:
                return false;
        }
    }

    private static ResolveInfo result(int kind, ComponentInfo component, long flags, ApplicationInfo app) {
        ResolveInfo info = new ResolveInfo();
        switch (kind) {
            case QUERY_SERVICES:
                info.serviceInfo = serviceInfo((ServiceInfo) component, flags, app);
                break;
            case QUERY_PROVIDERS:
                info.providerInfo = providerInfo((ProviderInfo) component, flags, app);
                break;
            default:
                info.activityInfo = activityInfo((ActivityInfo) component, flags, app);
                break;
        }
        return info;
    }

    private boolean named(ComponentInfo info, ComponentName component) {
        return packageName.equals(component.getPackageName()) && info.name.equals(component.getClassName());
    }

    /** A component disabled in the manifest only with MATCH_DISABLED_COMPONENTS, as Android's user state matches. */
    private static boolean matches(ComponentInfo info, long flags) {
        return info.enabled || (flags & PackageManager.MATCH_DISABLED_COMPONENTS) != 0;
    }

    private static ActivityInfo[] activities(List<ActivityInfo> list, long flags, ApplicationInfo app) {
        List<ActivityInfo> out = new ArrayList<>();
        for (ActivityInfo activity : list) {
            if (matches(activity, flags)) {
                out.add(activityInfo(activity, flags, app));
            }
        }
        return out.isEmpty() ? null : out.toArray(new ActivityInfo[0]);
    }

    private static ActivityInfo activityInfo(ActivityInfo template, long flags, ApplicationInfo app) {
        ActivityInfo info = new ActivityInfo(template);
        component(info, flags, app);
        return info;
    }

    private static ServiceInfo serviceInfo(ServiceInfo template, long flags, ApplicationInfo app) {
        ServiceInfo info = new ServiceInfo(template);
        component(info, flags, app);
        return info;
    }

    private static ProviderInfo providerInfo(ProviderInfo template, long flags, ApplicationInfo app) {
        ProviderInfo info = new ProviderInfo(template);
        component(info, flags, app);
        if ((flags & PackageManager.GET_URI_PERMISSION_PATTERNS) == 0) {
            info.uriPermissionPatterns = null;
        }
        return info;
    }

    private static void component(ComponentInfo info, long flags, ApplicationInfo app) {
        info.applicationInfo = app;
        if ((flags & PackageManager.GET_META_DATA) == 0) {
            info.metaData = null;
        }
    }

    private static PermissionInfo permissionInfo(PermissionInfo template, long flags) {
        PermissionInfo info = new PermissionInfo(template);
        if ((flags & PackageManager.GET_META_DATA) == 0) {
            info.metaData = null;
        }
        return info;
    }

    // Permissions.

    // Permission groups, instrumentation and the lists of the package manager's stubs (P4).

    /**
     * Every group the app sees: framework-res's, then the app's own, one entry per name as PermissionManagerService
     * keeps them: a group the platform has stays the platform's (Android refuses to install a package that redeclares
     * one, so that is only a guard), and of a name the app declares twice the last wins [duplicates measured:
     * package-answers-test on apk-runner-35].
     */
    List<PermissionGroupInfo> permissionGroups(long flags) {
        Map<String, PermissionGroupInfo> groups = new LinkedHashMap<>();
        synchronized (ManifestPackage.class) {
            platformPermissions();
            groups.putAll(sPlatformGroups);
        }
        for (PermissionGroupInfo group : mPermissionGroups) {
            if (!sPlatformGroups.containsKey(group.name)) {
                groups.put(group.name, group);
            }
        }
        List<PermissionGroupInfo> list = new ArrayList<>();
        for (PermissionGroupInfo group : groups.values()) {
            list.add(permissionGroupInfo(group, flags));
        }
        return list;
    }

    /**
     * One group by name, the platform's before the app's (a package cannot declare a group the platform has); null for
     * one nobody declares.
     */
    PermissionGroupInfo permissionGroup(String name, long flags) {
        PermissionGroupInfo template;
        synchronized (ManifestPackage.class) {
            platformPermissions();
            template = sPlatformGroups.get(name);
        }
        for (int i = mPermissionGroups.size() - 1; template == null && i >= 0; i--) {
            if (mPermissionGroups.get(i).name.equals(name)) {
                template = mPermissionGroups.get(i);
            }
        }
        return template != null ? permissionGroupInfo(template, flags) : null;
    }

    /** The permissions of one group, or of none for a null name: the app's own, then the platform's. */
    List<PermissionInfo> permissionsOfGroup(String group, long flags) {
        List<PermissionInfo> permissions = new ArrayList<>();
        List<PermissionInfo> all = new ArrayList<>(mPermissions);
        synchronized (ManifestPackage.class) {
            all.addAll(platformPermissions().values());
        }
        for (PermissionInfo permission : all) {
            if (Objects.equals(permission.group, group)) {
                permissions.add(permissionInfo(permission, flags));
            }
        }
        return permissions;
    }

    InstrumentationInfo instrumentation(ComponentName component, long flags) {
        for (InstrumentationInfo info : mInstrumentations) {
            if (packageName.equals(component.getPackageName()) && info.name.equals(component.getClassName())) {
                return instrumentationInfo(info, flags, applicationInfo(flags));
            }
        }
        return null;
    }

    /** The instrumentation that targets `targetPackage`, or all of them for a null one. */
    List<InstrumentationInfo> instrumentations(String targetPackage, long flags) {
        List<InstrumentationInfo> list = new ArrayList<>();
        ApplicationInfo app = applicationInfo(flags);
        for (InstrumentationInfo info : mInstrumentations) {
            if (targetPackage == null || targetPackage.equals(info.targetPackage)) {
                list.add(instrumentationInfo(info, flags, app));
            }
        }
        return list;
    }

    /** The app's providers of one process of one uid, as the package manager's queryContentProviders lists them. */
    List<ProviderInfo> providers(String processName, int uid, String metaDataKey, long flags) {
        List<ProviderInfo> list = new ArrayList<>();
        ApplicationInfo app = applicationInfo(flags);
        for (ProviderInfo provider : mProviders) {
            if (!matches(provider, flags)) {
                continue;
            }
            if (processName != null && !(processName.equals(provider.processName)
                    && android.os.UserHandle.isSameApp(app.uid, uid))) {
                continue;
            }
            if (metaDataKey != null && (provider.metaData == null || !provider.metaData.containsKey(metaDataKey))) {
                continue;
            }
            list.add(providerInfo(provider, flags, app));
        }
        return list;
    }

    private static PermissionGroupInfo permissionGroupInfo(PermissionGroupInfo template, long flags) {
        PermissionGroupInfo info = new PermissionGroupInfo(template);
        if ((flags & PackageManager.GET_META_DATA) == 0) {
            info.metaData = null;
        }
        return info;
    }

    private static InstrumentationInfo instrumentationInfo(InstrumentationInfo template, long flags,
            ApplicationInfo app) {
        InstrumentationInfo info = new InstrumentationInfo(template);
        info.sourceDir = app.sourceDir;
        info.publicSourceDir = app.publicSourceDir;
        info.splitNames = app.splitNames;
        info.splitSourceDirs = app.splitSourceDirs;
        info.splitPublicSourceDirs = app.splitPublicSourceDirs;
        info.dataDir = app.dataDir;
        info.deviceProtectedDataDir = app.deviceProtectedDataDir;
        info.credentialProtectedDataDir = app.credentialProtectedDataDir;
        info.primaryCpuAbi = app.primaryCpuAbi;
        info.secondaryCpuAbi = app.secondaryCpuAbi;
        info.nativeLibraryDir = app.nativeLibraryDir;
        info.secondaryNativeLibraryDir = app.secondaryNativeLibraryDir;
        if ((flags & PackageManager.GET_META_DATA) == 0) {
            info.metaData = null;
        }
        return info;
    }

    /** getPermissionInfo's answer: the app's own permission or the platform's, or null for one nobody declares. */
    PermissionInfo permissionInfo(String name, long flags) {
        PermissionInfo template = ownPermission(name);
        if (template == null) {
            template = platformPermissions().get(name);
        }
        return template != null ? permissionInfo(template, flags) : null;
    }

    private PermissionInfo ownPermission(String name) {
        for (PermissionInfo permission : mPermissions) {
            if (permission.name.equals(name)) {
                return permission;
            }
        }
        return null;
    }

    /**
     * Whether the app holds the permission: PermissionPolicy's answer (ADR-0045 decision 6), with what the platform's
     * splits pass on. Each denial is logged once, and a grant that departs from Android once per app.
     */
    boolean granted(String permission) {
        PermissionPolicy.Decision decision = PermissionPolicy.decide(policyApp(), permission);
        synchronized (mLoggedDenials) {
            if (decision.grant == PermissionPolicy.Grant.GRANTED) {
                if (!decision.departure.isEmpty() && !mDepartureLogged) {
                    mDepartureLogged = true;
                    Slog.i(POLICY_TAG, packageName + ": " + decision.departure
                            + " (ADR-0045 decision 6, a logged departure)");
                }
                return true;
            }
            if (mLoggedDenials.add(permission)) {
                Slog.i(POLICY_TAG, packageName + " is denied " + permission + ": " + decision.reason);
            }
        }
        return false;
    }

    /** The app as PermissionPolicy sees it, made on first use, since it reads the platform's permissions. */
    private synchronized PermissionPolicy.App policyApp() {
        if (mPolicyApp == null) {
            Set<String> declared = new HashSet<>();
            for (PermissionInfo permission : mPermissions) {
                declared.add(permission.name);
            }
            // Each new permission with the permissions split into it, whatever the split's targetSdk, as
            // setInitialGrantForNewImplicitPermissionsLocked maps them.
            Map<String, List<String>> sources = new HashMap<>();
            for (Split split : splits()) {
                for (String permission : split.newPermissions) {
                    sources.computeIfAbsent(permission, name -> new ArrayList<>()).add(split.permission);
                }
            }
            mPolicyApp = new PermissionPolicy.App(mApplication.targetSdkVersion, new HashSet<>(mRequestedPermissions),
                    Collections.unmodifiableSet(mImplicitPermissions), declared,
                    name -> sources.getOrDefault(name, Collections.emptyList()), this::protectionLevel);
        }
        return mPolicyApp;
    }

    /** The permission's PermissionInfo.protectionLevel, the app's own or the platform's; UNKNOWN for neither. */
    private int protectionLevel(String permission) {
        PermissionInfo info = ownPermission(permission);
        if (info == null) {
            info = platformPermissions().get(permission);
        }
        return info != null ? info.protectionLevel : PermissionPolicy.UNKNOWN;
    }
}
