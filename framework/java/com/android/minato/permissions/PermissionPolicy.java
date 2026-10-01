package com.android.minato.permissions;

import java.util.Set;

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
 *       alone in Minato; every other dangerous one is denied.</li>
 * </ol>
 * Every denial is "denied, don't ask again", and shouldShowRequestPermissionRationale is false. A denial carries
 * its reason for the log line; a grant carries none. Plain Java, so tools/framework-host-check.sh runs it on the
 * host's JVM.
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

    public enum Grant { GRANTED, DENIED_FIXED }

    public static final class Decision {
        public final Grant grant;
        /** Why it is denied, for the log; empty for a grant. */
        public final String reason;

        private Decision(Grant grant, String reason) {
            this.grant = grant;
            this.reason = reason;
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

    private static final Decision GRANTED = new Decision(Grant.GRANTED, "");

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
        return denied("a dangerous permission, which nothing in M4 backs");
    }

    /** Every denial is fixed, so Android would show no rationale either. */
    public static boolean shouldShowRequestPermissionRationale(String permission) {
        return false;
    }

    private static Decision denied(String reason) {
        return new Decision(Grant.DENIED_FIXED, reason);
    }
}
