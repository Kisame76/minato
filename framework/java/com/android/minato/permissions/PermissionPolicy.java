package com.android.minato.permissions;

import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.function.Function;
import java.util.function.ToIntFunction;

/**
 * Runtime permissions answered at once, without a dialog (ADR-0045 decision 6), by a permission's protection level
 * as PermissionInfo encodes it (base in the low four bits, flags above), the app's targetSdk, and whether the app
 * requests and declares it. In this order of precedence:
 * <ol>
 *   <li>a permission the app does not request, or that no package declares, is denied;</li>
 *   <li>the app's own permissions are granted;</li>
 *   <li>denied at every targetSdk: signature and internal permissions (Android never grants the platform's to an
 *       app without the platform's key), the special ones (overlay, settings writes, installing, all-files access)
 *       and POST_NOTIFICATIONS, whose prompt Android does not start with the answer fixed;</li>
 *   <li>below targetSdk 23, Android's old model grants what the app requests;</li>
 *   <li>normal permissions are granted; storage up to targetSdk 32, since /storage/emulated/0 belongs to the app
 *       alone in Minato; the location group is the host's to decide ({@link Grant#ASK_HOST}, ADR-0063 decision 2);
 *       every other dangerous one is denied.</li>
 * </ol>
 * A runtime permission that a split added is granted when a permission it was split from is granted
 * ({@link #decide(App, String)}). Every denial is "denied, don't ask again", and
 * shouldShowRequestPermissionRationale is false. A denial carries its reason for the log line; a grant carries none,
 * unless it departs from Android, which its departure names. Plain Java, so tools/framework-host-check.sh runs it
 * on the host's JVM.
 */
public final class PermissionPolicy {
    /** PermissionInfo's bases. */
    public static final int PROTECTION_NORMAL = 0;
    public static final int PROTECTION_DANGEROUS = 1;
    public static final int PROTECTION_SIGNATURE = 2;
    public static final int PROTECTION_INTERNAL = 4;
    public static final int PROTECTION_MASK_BASE = 0xf;
    /** A permission no package declares. */
    public static final int UNKNOWN = -1;

    /** The groups the host decides; the value of APKRT_REQUEST_PERMISSION's group field (ADR-0063 decision 2). */
    public static final int GROUP_LOCATION = 1;
    public static final int GROUP_MICROPHONE = 2;
    /** ADR-0073 decision 8: APKRT_PERMISSION_CAMERA. */
    public static final int GROUP_CAMERA = 3;

    /**
     * GRANTED and DENIED_FIXED are final. ASK_HOST is a dangerous permission of a group the host decides: the answer
     * is what the host last said for the group ({@link PermissionAnswers}), denied before the first.
     */
    public enum Grant { GRANTED, DENIED_FIXED, ASK_HOST }

    public static final class Decision {
        public final Grant grant;
        /** Why it is denied, for the log; empty for a grant. */
        public final String reason;
        /** How a grant departs from what Android 15 gives at an install, for the log once per app; else empty. */
        public final String departure;

        private Decision(Grant grant, String reason, String departure) {
            this.grant = grant;
            this.reason = reason;
            this.departure = departure;
        }
    }

    /** What an app's answers depend on beyond each permission's own: its requests and the platform's splits. */
    public static final class App {
        final int targetSdk;
        final Set<String> requested;
        final Set<String> implicit;
        final Set<String> declared;
        final Function<String, List<String>> sources;
        final ToIntFunction<String> levels;

        /**
         * @param requested every permission the app requests, the implicit ones among them
         * @param implicit  those that compatibility and split permissions added
         * @param declared  those the app's manifest declares
         * @param sources   for a permission, the permissions of every split that names it as a new permission
         * @param levels    a permission's PermissionInfo.protectionLevel, or {@link #UNKNOWN}
         */
        public App(int targetSdk, Set<String> requested, Set<String> implicit, Set<String> declared,
                   Function<String, List<String>> sources, ToIntFunction<String> levels) {
            this.targetSdk = targetSdk;
            this.requested = requested;
            this.implicit = implicit;
            this.declared = declared;
            this.sources = sources;
            this.levels = levels;
        }
    }

    private static final Set<String> SPECIAL = Set.of("android.permission.SYSTEM_ALERT_WINDOW",
            "android.permission.WRITE_SETTINGS", "android.permission.REQUEST_INSTALL_PACKAGES",
            "android.permission.MANAGE_EXTERNAL_STORAGE");
    private static final String POST_NOTIFICATIONS = "android.permission.POST_NOTIFICATIONS";
    private static final Set<String> STORAGE = Set.of("android.permission.READ_EXTERNAL_STORAGE",
            "android.permission.WRITE_EXTERNAL_STORAGE");
    /** Android 6's runtime model; below it, requested permissions are granted at install. */
    private static final int RUNTIME_MODEL_SDK = 23;
    /** The last targetSdk whose storage permissions Minato grants (Android 13 replaced them). */
    private static final int LAST_STORAGE_SDK = 32;

    private static final String ACCESS_FINE_LOCATION = "android.permission.ACCESS_FINE_LOCATION";
    private static final String ACCESS_COARSE_LOCATION = "android.permission.ACCESS_COARSE_LOCATION";
    private static final String RECORD_AUDIO = "android.permission.RECORD_AUDIO";
    private static final String CAMERA = "android.permission.CAMERA";
    private static final String ACCESS_BACKGROUND_LOCATION = "android.permission.ACCESS_BACKGROUND_LOCATION";

    private static final Decision GRANTED = new Decision(Grant.GRANTED, "", "");
    private static final Decision ASK_HOST = new Decision(Grant.ASK_HOST, "", "");

    private PermissionPolicy() {
    }

    /**
     * @param protectionLevel the permission's PermissionInfo.protectionLevel, or {@link #UNKNOWN}
     * @param requested       the app's manifest has a uses-permission for it
     * @param declaredByApp   the app's manifest declares it with a permission element
     */
    public static Decision decide(String permission, int protectionLevel, int targetSdk, boolean requested,
                                  boolean declaredByApp) {
        if (!requested) {
            return denied("not requested in the manifest");
        }
        if (declaredByApp) {
            return GRANTED;
        }
        if (protectionLevel == UNKNOWN) {
            return denied("no package declares it");
        }
        int base = protectionLevel & PROTECTION_MASK_BASE;
        if (base == PROTECTION_SIGNATURE || base == PROTECTION_INTERNAL) {
            return denied("a platform signature permission");
        }
        if (SPECIAL.contains(permission)) {
            return denied("a special permission, which M4 does not give");
        }
        if (permission.equals(POST_NOTIFICATIONS)) {
            return denied("notifications count as off");
        }
        if (base != PROTECTION_NORMAL && base != PROTECTION_DANGEROUS) {
            return denied("an unknown protection level " + protectionLevel);
        }
        if (targetSdk < RUNTIME_MODEL_SDK || base == PROTECTION_NORMAL) {
            return GRANTED;
        }
        if (STORAGE.contains(permission)) {
            return targetSdk <= LAST_STORAGE_SDK ? GRANTED : denied("storage from targetSdk 33 on, as Android 15");
        }
        if (hostGroup(permission) != 0) {
            return ASK_HOST;
        }
        if (permission.equals(ACCESS_BACKGROUND_LOCATION)) {
            return denied("background location is not offered (ADR-0063 decision 2)");
        }
        return denied("a dangerous permission, which nothing in M4 backs");
    }

    /** The host's group (GROUP_LOCATION ...) that decides the permission, or 0 for a permission the host does not. */
    public static int hostGroup(String permission) {
        if (RECORD_AUDIO.equals(permission)) {
            return GROUP_MICROPHONE;
        }
        if (CAMERA.equals(permission)) {
            return GROUP_CAMERA;
        }
        return ACCESS_FINE_LOCATION.equals(permission) || ACCESS_COARSE_LOCATION.equals(permission)
                ? GROUP_LOCATION : 0;
    }

    /**
     * The permission's answer for the app: its own decision, and for a runtime permission that a split added, a
     * permission it was split from passes its grant on (decision 6 as accepted 2026-10-03).
     * <ul>
     *   <li>From a source granted at the install, such as BLUETOOTH, that is Android's rule at an install
     *       (PermissionManagerServiceImpl.setInitialGrantForNewImplicitPermissionsLocked and
     *       inheritPermissionStateToNewImplicitPermissionLocked).</li>
     *   <li>From a runtime source, such as storage, Android passes the grant on only at an update; Minato's storage
     *       grant stands for the user's earlier yes, and the decision names the departure.</li>
     * </ul>
     * A source that is denied, or that the app does not request, passes nothing on; through a chain of splits a
     * source is decided as the permission is, and a cycle ends.
     */
    public static Decision decide(App app, String permission) {
        return decide(app, permission, new HashSet<>());
    }

    private static Decision decide(App app, String permission, Set<String> deciding) {
        int level = app.levels.applyAsInt(permission);
        Decision own = decide(permission, level, app.targetSdk, app.requested.contains(permission),
                app.declared.contains(permission));
        if (own.grant != Grant.DENIED_FIXED || !app.implicit.contains(permission)
                || (level & PROTECTION_MASK_BASE) != PROTECTION_DANGEROUS || !deciding.add(permission)) {
            return own;
        }
        try {
            String runtimeSource = null;
            for (String source : app.sources.apply(permission)) {
                if (decide(app, source, deciding).grant != Grant.GRANTED) {
                    continue;
                }
                if ((app.levels.applyAsInt(source) & PROTECTION_MASK_BASE) != PROTECTION_DANGEROUS) {
                    return GRANTED;
                }
                if (runtimeSource == null) {
                    runtimeSource = source;
                }
            }
            return runtimeSource == null ? own : new Decision(Grant.GRANTED, "", permission + " takes the grant of "
                    + runtimeSource + ", a runtime permission, which Android 15 passes on only at an update");
        } finally {
            deciding.remove(permission);
        }
    }

    /** Every denial of a permission the host does not decide is fixed, so Android would show no rationale either. */
    public static boolean shouldShowRequestPermissionRationale(String permission) {
        return false;
    }

    private static Decision denied(String reason) {
        return new Decision(Grant.DENIED_FIXED, reason, "");
    }
}
