package com.android.minato.permissions;

import com.android.minato.permissions.PermissionPolicy.Decision;
import com.android.minato.permissions.PermissionPolicy.Grant;

import java.lang.reflect.Method;

/**
 * ADR-0045 decision 6 on the host's JVM (tools/framework-host-check.sh): each runtime permission answered at once,
 * by its protection level and the app's targetSdk, in the policy's order of precedence:
 * <ol>
 *   <li>a permission the app does not request, or that no package declares, is denied;</li>
 *   <li>the app's own permissions are granted;</li>
 *   <li>signature (and internal) permissions, the special ones and POST_NOTIFICATIONS are denied at every
 *       targetSdk;</li>
 *   <li>below targetSdk 23, Android's old model grants what the app requests;</li>
 *   <li>normal permissions are granted, storage up to targetSdk 32, every other dangerous one is denied.</li>
 * </ol>
 */
public final class PermissionPolicyTest {
    private static final int NORMAL = PermissionPolicy.PROTECTION_NORMAL;
    private static final int DANGEROUS = PermissionPolicy.PROTECTION_DANGEROUS;
    private static final int SIGNATURE = PermissionPolicy.PROTECTION_SIGNATURE;
    private static final int INTERNAL = PermissionPolicy.PROTECTION_INTERNAL;
    // PermissionInfo's flags, as framework-res's manifest combines them with a base.
    private static final int PRIVILEGED = 0x10;
    private static final int APPOP = 0x40;
    private static final int DEVELOPMENT = 0x20;

    private static Grant grant(String permission, int level, int targetSdk) {
        return PermissionPolicy.decide(permission, level, targetSdk, true, false).grant;
    }

    // 1. Requested and declared.

    public void testNotRequestedIsDenied() {
        Decision decision = PermissionPolicy.decide("android.permission.INTERNET", NORMAL, 35, false, false);
        check(decision.grant == Grant.DENIED_FIXED, "an unrequested normal permission");
        check(decision.reason.contains("not requested"), "reason: " + decision.reason);
    }

    public void testUndeclaredIsDenied() {
        Decision nobody = PermissionPolicy.decide("com.example.NOBODY", PermissionPolicy.UNKNOWN, 35, true, false);
        check(nobody.grant == Grant.DENIED_FIXED, "no package declares it");
        check(nobody.reason.contains("no package declares"), "reason: " + nobody.reason);
        check(grant("com.example.NOBODY", PermissionPolicy.UNKNOWN, 22) == Grant.DENIED_FIXED, "below 23 too");
    }

    // 2. The app's own.

    public void testOwnPermissionsAreGranted() {
        Decision own = PermissionPolicy.decide("com.example.app.permission.C2D", SIGNATURE, 35, true, true);
        check(own.grant == Grant.GRANTED, "the app's own signature permission");
        check(PermissionPolicy.decide("com.example.app.DANGER", DANGEROUS, 35, true, true).grant == Grant.GRANTED,
                "the app's own dangerous permission");
        check(PermissionPolicy.decide("com.example.app.OWN", NORMAL, 35, false, true).grant == Grant.DENIED_FIXED,
                "its own, not requested");
    }

    // 3. Denied at every targetSdk.

    public void testSignatureAndInternalAreDeniedAtEveryTargetSdk() {
        for (int targetSdk : new int[] {1, 22, 23, 32, 33, 35}) {
            check(grant("android.permission.READ_LOGS", SIGNATURE | PRIVILEGED | DEVELOPMENT, targetSdk)
                    == Grant.DENIED_FIXED, "signature at " + targetSdk);
            Decision internal = PermissionPolicy.decide("android.permission.MANAGE_ROLLBACKS", INTERNAL | PRIVILEGED,
                    targetSdk, true, false);
            check(internal.grant == Grant.DENIED_FIXED, "internal at " + targetSdk);
            check(internal.reason.contains("platform"), "internal's reason: " + internal.reason);
        }
    }

    public void testSpecialOnesAreDeniedAtEveryTargetSdk() {
        String[] special = {"android.permission.SYSTEM_ALERT_WINDOW", "android.permission.WRITE_SETTINGS",
                "android.permission.REQUEST_INSTALL_PACKAGES", "android.permission.MANAGE_EXTERNAL_STORAGE"};
        for (String permission : special) {
            for (int targetSdk : new int[] {22, 23, 35}) {
                // Their levels in framework-res: signature with appop and others; checked by name either way.
                check(grant(permission, SIGNATURE | APPOP, targetSdk) == Grant.DENIED_FIXED,
                        permission + " at " + targetSdk);
                check(grant(permission, NORMAL, targetSdk) == Grant.DENIED_FIXED,
                        permission + " by name at " + targetSdk);
            }
        }
    }

    public void testPostNotificationsIsDeniedAtEveryTargetSdk() {
        for (int targetSdk : new int[] {22, 23, 32, 33, 35}) {
            check(grant("android.permission.POST_NOTIFICATIONS", DANGEROUS, targetSdk) == Grant.DENIED_FIXED,
                    "POST_NOTIFICATIONS at " + targetSdk);
        }
    }

    // 4. Android's old model below targetSdk 23.

    public void testBelow23GrantsWhatIsRequested() {
        check(grant("android.permission.CAMERA", DANGEROUS, 22) == Grant.GRANTED, "dangerous at 22");
        check(grant("android.permission.ACCESS_FINE_LOCATION", DANGEROUS, 1) == Grant.GRANTED, "dangerous at 1");
        check(grant("android.permission.INTERNET", NORMAL, 22) == Grant.GRANTED, "normal at 22");
        check(grant("android.permission.CAMERA", DANGEROUS, 23) == Grant.DENIED_FIXED, "dangerous at 23");
    }

    // 5. Normal, storage and dangerous from targetSdk 23.

    public void testNormalIsGranted() {
        check(grant("android.permission.INTERNET", NORMAL, 35) == Grant.GRANTED, "INTERNET");
        check(grant("android.permission.VIBRATE", NORMAL, 23) == Grant.GRANTED, "VIBRATE");
    }

    public void testStorageUpTo32() {
        for (String permission : new String[] {"android.permission.READ_EXTERNAL_STORAGE",
                "android.permission.WRITE_EXTERNAL_STORAGE"}) {
            check(grant(permission, DANGEROUS, 23) == Grant.GRANTED, permission + " at 23");
            check(grant(permission, DANGEROUS, 32) == Grant.GRANTED, permission + " at 32");
            check(grant(permission, DANGEROUS, 33) == Grant.DENIED_FIXED, permission + " at 33");
            check(grant(permission, DANGEROUS, 35) == Grant.DENIED_FIXED, permission + " at 35");
        }
    }

    public void testOtherDangerousAreDenied() {
        String[] dangerous = {"android.permission.RECORD_AUDIO", "android.permission.ACCESS_FINE_LOCATION",
                "android.permission.CAMERA", "android.permission.READ_CONTACTS",
                "android.permission.READ_MEDIA_IMAGES"};
        for (String permission : dangerous) {
            Decision decision = PermissionPolicy.decide(permission, DANGEROUS, 35, true, false);
            check(decision.grant == Grant.DENIED_FIXED, permission);
            check(!decision.reason.isEmpty(), permission + " has a reason for the log");
        }
    }

    public void testUnknownBaseIsDenied() {
        check(grant("com.example.ODD", 0xe, 35) == Grant.DENIED_FIXED, "a base PermissionInfo does not define");
    }

    public void testGrantsHaveNoReasonDenialsDo() {
        check(PermissionPolicy.decide("android.permission.INTERNET", NORMAL, 35, true, false).reason.isEmpty(),
                "a grant logs nothing");
        check(!PermissionPolicy.decide("android.permission.CAMERA", DANGEROUS, 35, true, false).reason.isEmpty(),
                "a denial names its reason");
    }

    public void testRationaleIsNeverShown() {
        check(!PermissionPolicy.shouldShowRequestPermissionRationale("android.permission.CAMERA"), "CAMERA");
        check(!PermissionPolicy.shouldShowRequestPermissionRationale("android.permission.INTERNET"), "INTERNET");
    }

    // The runner, as ServiceStubTest's.

    private static void check(boolean condition, String what) {
        if (!condition) {
            throw new AssertionError(what);
        }
    }

    public static void main(String[] args) throws Exception {
        int failed = 0, run = 0;
        for (Method test : PermissionPolicyTest.class.getDeclaredMethods()) {
            if (!test.getName().startsWith("test")) {
                continue;
            }
            run++;
            try {
                test.invoke(new PermissionPolicyTest());
            } catch (java.lang.reflect.InvocationTargetException thrown) {
                failed++;
                System.out.println("FAIL " + test.getName() + ": " + thrown.getCause());
            }
        }
        if (failed > 0) {
            System.out.println("PermissionPolicyTest: " + failed + " of " + run + " failed");
            System.exit(1);
        }
        System.out.println("PermissionPolicyTest: " + run + " of " + run + " fine");
    }
}
