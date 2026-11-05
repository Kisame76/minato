package com.android.minato.permissions;

import com.android.minato.permissions.PermissionPolicy.Decision;
import com.android.minato.permissions.PermissionPolicy.Grant;

import java.lang.reflect.Method;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

/**
 * ADR-0045 decision 6 on the host's JVM (tools/framework-host-check.sh): each runtime permission answered at once,
 * by its protection level and the app's targetSdk, in the policy's order of precedence:
 * <ol>
 *   <li>a permission the app does not request, or that no package declares, is denied;</li>
 *   <li>the app's own permissions are granted;</li>
 *   <li>signature (and internal) permissions and the special ones are denied at every targetSdk, and
 *       POST_NOTIFICATIONS below targetSdk 33;</li>
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

    public void testPostNotificationsIsDeniedBelowTargetSdk33() {
        // Android 13's own prompt for older apps, at their first channel, is a step of its own (ADR-0074 N1b).
        for (int targetSdk : new int[] {22, 23, 32}) {
            check(grant("android.permission.POST_NOTIFICATIONS", DANGEROUS, targetSdk) == Grant.DENIED_FIXED,
                    "POST_NOTIFICATIONS at " + targetSdk);
        }
    }

    // 4. Android's old model below targetSdk 23.

    public void testBelow23GrantsWhatIsRequested() {
        check(grant("android.permission.CAMERA", DANGEROUS, 22) == Grant.GRANTED, "dangerous at 22");
        check(grant("android.permission.ACCESS_FINE_LOCATION", DANGEROUS, 1) == Grant.GRANTED, "dangerous at 1");
        check(grant("android.permission.INTERNET", NORMAL, 22) == Grant.GRANTED, "normal at 22");
        check(grant("android.permission.READ_CONTACTS", DANGEROUS, 23) == Grant.DENIED_FIXED, "dangerous at 23");
    }

    // ADR-0063 decision 2: the location group is the host's to decide (the first ASK_HOST).

    public void testLocationAsksTheHost() {
        for (String permission : new String[] {"android.permission.ACCESS_FINE_LOCATION",
                "android.permission.ACCESS_COARSE_LOCATION"}) {
            for (int targetSdk : new int[] {23, 30, 35}) {
                Decision decision = PermissionPolicy.decide(permission, DANGEROUS, targetSdk, true, false);
                check(decision.grant == Grant.ASK_HOST, permission + " at " + targetSdk);
                check(decision.reason.isEmpty() && decision.departure.isEmpty(), permission + " logs nothing");
            }
            check(grant(permission, DANGEROUS, 22) == Grant.GRANTED, permission + " below 23 is Android's old model");
            check(PermissionPolicy.decide(permission, DANGEROUS, 35, false, false).grant == Grant.DENIED_FIXED,
                    permission + " not requested");
        }
    }

    public void testBackgroundLocationAndOtherGroupsStayDenied() {
        check(grant("android.permission.ACCESS_BACKGROUND_LOCATION", DANGEROUS, 35) == Grant.DENIED_FIXED,
                "background location is not offered (ADR-0063 decision 2)");
    }

    public void testNotificationsAreTheHostsToDecideFromAndroid13() {
        String post = "android.permission.POST_NOTIFICATIONS";
        check(grant(post, DANGEROUS, 35) == Grant.ASK_HOST, "POST_NOTIFICATIONS (ADR-0074 decision 1)");
        check(grant(post, DANGEROUS, 33) == Grant.ASK_HOST, "at 33");
        check(PermissionPolicy.hostGroup(post) == PermissionPolicy.GROUP_NOTIFICATIONS, "its group");
        check(PermissionPolicy.GROUP_NOTIFICATIONS == 4, "APKRT_PERMISSION_NOTIFICATIONS");
    }

    public void testTheCameraIsTheHostsToDecideToo() {
        check(grant("android.permission.CAMERA", DANGEROUS, 35) == Grant.ASK_HOST, "CAMERA (ADR-0073 decision 8)");
        check(grant("android.permission.CAMERA", DANGEROUS, 22) == Grant.GRANTED, "below 23 is Android's old model");
        check(PermissionPolicy.hostGroup("android.permission.CAMERA") == PermissionPolicy.GROUP_CAMERA, "its group");
        check(PermissionPolicy.GROUP_CAMERA == 3, "APKRT_PERMISSION_CAMERA");
    }

    public void testTheMicrophoneIsTheHostsToDecideToo() {
        check(grant("android.permission.RECORD_AUDIO", DANGEROUS, 35) == Grant.ASK_HOST, "RECORD_AUDIO (ADR-0064)");
        check(grant("android.permission.RECORD_AUDIO", DANGEROUS, 22) == Grant.GRANTED,
                "below 23 is Android's old model");
        check(PermissionPolicy.hostGroup("android.permission.RECORD_AUDIO") == PermissionPolicy.GROUP_MICROPHONE,
                "its group");
        check(PermissionPolicy.decide("android.permission.RECORD_AUDIO", DANGEROUS, 35, false, false).grant
                == Grant.DENIED_FIXED, "not requested");
    }

    public void testASplitLocationPermissionAsksTheHostToo() {
        // Android 9 and below ask for ACCESS_COARSE_LOCATION with ACCESS_FINE_LOCATION.
        PermissionPolicy.App app = app(28, List.of(P + "ACCESS_FINE_LOCATION"),
                List.of(P + "ACCESS_COARSE_LOCATION"));
        check(PermissionPolicy.decide(app, P + "ACCESS_FINE_LOCATION").grant == Grant.ASK_HOST, "fine");
        check(PermissionPolicy.decide(app, P + "ACCESS_COARSE_LOCATION").grant == Grant.ASK_HOST, "coarse");
    }

    public void testHostGroups() {
        check(PermissionPolicy.hostGroup(P + "ACCESS_FINE_LOCATION") == PermissionPolicy.GROUP_LOCATION, "fine");
        check(PermissionPolicy.hostGroup(P + "ACCESS_COARSE_LOCATION") == PermissionPolicy.GROUP_LOCATION, "coarse");
        check(PermissionPolicy.hostGroup(P + "ACCESS_BACKGROUND_LOCATION") == 0, "background");
        check(PermissionPolicy.hostGroup(P + "READ_CONTACTS") == 0, "contacts");
        check(PermissionPolicy.hostGroup(null) == 0, "null");
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
        String[] dangerous = {"android.permission.ACCESS_BACKGROUND_LOCATION",
                "android.permission.READ_CONTACTS",
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
        check(!PermissionPolicy.decide("android.permission.READ_CONTACTS", DANGEROUS, 35, true, false).reason.isEmpty(),
                "a denial names its reason");
    }

    // 6. Implicit permissions (decision 6 as accepted 2026-10-03): a runtime one a split added is granted when a
    // permission it was split from is granted. From an install-time source that is Android's own rule at an
    // install; from a runtime source (storage), Android's at an update, a logged departure.

    private static final String P = "android.permission.";
    // platform.xml's splits, as the test needs them: each new permission with the permissions split into it.
    private static final Map<String, List<String>> SPLITS = Map.of(
            P + "BLUETOOTH_SCAN", List.of(P + "BLUETOOTH", P + "BLUETOOTH_ADMIN"),
            P + "BLUETOOTH_CONNECT", List.of(P + "BLUETOOTH", P + "BLUETOOTH_ADMIN"),
            P + "ACCESS_COARSE_LOCATION", List.of(P + "ACCESS_FINE_LOCATION"),
            P + "ACCESS_BACKGROUND_LOCATION", List.of(P + "ACCESS_FINE_LOCATION", P + "ACCESS_COARSE_LOCATION"),
            P + "ACCESS_MEDIA_LOCATION", List.of(P + "READ_EXTERNAL_STORAGE", P + "WRITE_EXTERNAL_STORAGE"),
            P + "READ_MEDIA_IMAGES", List.of(P + "READ_EXTERNAL_STORAGE", P + "WRITE_EXTERNAL_STORAGE"),
            P + "READ_MEDIA_VISUAL_USER_SELECTED",
                    List.of(P + "READ_MEDIA_IMAGES", P + "READ_MEDIA_VIDEO", P + "ACCESS_MEDIA_LOCATION"));
    private static final Map<String, Integer> LEVELS = Map.of(P + "BLUETOOTH", NORMAL, P + "BLUETOOTH_ADMIN", NORMAL,
            P + "READ_LOGS", SIGNATURE);

    /** An app of targetSdk that requests explicit and has implicit added. */
    private static PermissionPolicy.App app(int targetSdk, List<String> explicit, List<String> implicit) {
        Set<String> requested = new HashSet<>(explicit);
        requested.addAll(implicit);
        // Every other permission of the test is dangerous.
        return new PermissionPolicy.App(targetSdk, requested, new HashSet<>(implicit), Set.of(),
                name -> SPLITS.getOrDefault(name, List.of()), name -> LEVELS.getOrDefault(name, DANGEROUS));
    }

    public void testBluetoothFromAnInstallTimeSource() {
        PermissionPolicy.App app = app(28, List.of(P + "BLUETOOTH"),
                List.of(P + "BLUETOOTH_SCAN", P + "BLUETOOTH_CONNECT"));
        Decision scan = PermissionPolicy.decide(app, P + "BLUETOOTH_SCAN");
        check(scan.grant == Grant.GRANTED, "BLUETOOTH_SCAN from BLUETOOTH");
        check(scan.departure.isEmpty(), "Android's own rule: " + scan.departure);
        check(PermissionPolicy.decide(app, P + "BLUETOOTH_CONNECT").grant == Grant.GRANTED, "BLUETOOTH_CONNECT");
        check(PermissionPolicy.decide(app, P + "BLUETOOTH").grant == Grant.GRANTED, "BLUETOOTH, normal");
    }

    public void testStorageToMediaWithTheChainIsALoggedDeparture() {
        PermissionPolicy.App app = app(28, List.of(P + "READ_EXTERNAL_STORAGE"), List.of(P + "ACCESS_MEDIA_LOCATION",
                P + "READ_MEDIA_IMAGES", P + "READ_MEDIA_VISUAL_USER_SELECTED"));
        check(PermissionPolicy.decide(app, P + "READ_EXTERNAL_STORAGE").grant == Grant.GRANTED, "storage at 28");
        for (String media : new String[] {"ACCESS_MEDIA_LOCATION", "READ_MEDIA_IMAGES"}) {
            Decision decision = PermissionPolicy.decide(app, P + media);
            check(decision.grant == Grant.GRANTED, media + " from storage");
            check(decision.departure.contains(P + "READ_EXTERNAL_STORAGE"), media + "'s departure: "
                    + decision.departure);
        }
        Decision chain = PermissionPolicy.decide(app, P + "READ_MEDIA_VISUAL_USER_SELECTED");
        check(chain.grant == Grant.GRANTED, "VISUAL_USER_SELECTED through READ_MEDIA_IMAGES");
        check(!chain.departure.isEmpty(), "the chain is the departure too");
    }

    public void testAnImplicitPermissionGrantedOnItsOwnIsNoDeparture() {
        // WRITE_EXTERNAL_STORAGE splits into READ_EXTERNAL_STORAGE at every targetSdk; storage is granted up to 32.
        Map<String, List<String>> splits = Map.of(P + "READ_EXTERNAL_STORAGE", List.of(P + "WRITE_EXTERNAL_STORAGE"));
        PermissionPolicy.App app = new PermissionPolicy.App(28,
                Set.of(P + "WRITE_EXTERNAL_STORAGE", P + "READ_EXTERNAL_STORAGE"), Set.of(P + "READ_EXTERNAL_STORAGE"),
                Set.of(), name -> splits.getOrDefault(name, List.of()), name -> DANGEROUS);
        Decision read = PermissionPolicy.decide(app, P + "READ_EXTERNAL_STORAGE");
        check(read.grant == Grant.GRANTED && read.departure.isEmpty(), "storage's own grant: " + read.departure);
    }

    public void testFromADeniedSourceStaysDenied() {
        PermissionPolicy.App app = app(28, List.of(P + "READ_CONTACTS"),
                List.of(P + "ACCESS_BACKGROUND_LOCATION"));
        for (String location : new String[] {"READ_CONTACTS", "ACCESS_BACKGROUND_LOCATION"}) {
            Decision decision = PermissionPolicy.decide(app, P + location);
            check(decision.grant == Grant.DENIED_FIXED, location);
            check(!decision.reason.isEmpty() && decision.departure.isEmpty(), location + ": " + decision.reason);
        }
        // Storage from targetSdk 33 on is denied, and so is what it would pass on.
        PermissionPolicy.App late = app(33, List.of(P + "READ_EXTERNAL_STORAGE"), List.of(P + "ACCESS_MEDIA_LOCATION"));
        check(PermissionPolicy.decide(late, P + "ACCESS_MEDIA_LOCATION").grant == Grant.DENIED_FIXED, "at 33");
    }

    public void testOnlyRuntimePermissionsInheritAndOnlyImplicitOnes() {
        // A signature permission a split added keeps its denial.
        Map<String, List<String>> splits = Map.of(P + "READ_LOGS", List.of(P + "BLUETOOTH"));
        PermissionPolicy.App app = new PermissionPolicy.App(28, Set.of(P + "BLUETOOTH", P + "READ_LOGS"),
                Set.of(P + "READ_LOGS"), Set.of(), name -> splits.getOrDefault(name, List.of()),
                name -> LEVELS.getOrDefault(name, DANGEROUS));
        check(PermissionPolicy.decide(app, P + "READ_LOGS").grant == Grant.DENIED_FIXED, "signature");
        // The same dangerous permission requested by the manifest itself is not implicit.
        PermissionPolicy.App explicit = app(28, List.of(P + "BLUETOOTH", P + "BLUETOOTH_SCAN"), List.of());
        check(PermissionPolicy.decide(explicit, P + "BLUETOOTH_SCAN").grant == Grant.DENIED_FIXED, "explicit");
    }

    public void testASplitCycleEnds() {
        Map<String, List<String>> splits = Map.of(P + "A", List.of(P + "B"), P + "B", List.of(P + "A"));
        PermissionPolicy.App app = new PermissionPolicy.App(28, Set.of(P + "A", P + "B"), Set.of(P + "A", P + "B"),
                Set.of(), name -> splits.getOrDefault(name, List.of()), name -> DANGEROUS);
        check(PermissionPolicy.decide(app, P + "A").grant == Grant.DENIED_FIXED, "a cycle grants nothing");
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
