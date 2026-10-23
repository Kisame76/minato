package com.android.minato.permissions;

import java.util.Collection;

/**
 * What the host answered for the permission groups it decides (ADR-0063 decision 2), kept for the process: the stored
 * answer checkSelfPermission reads, denied before the first; whether a request must go to the host; Android's
 * rationale after one denial and its "don't ask again" after the second, when the dialog no longer comes and the
 * answer is a denial at once. A solicited answer (the result of a request) counts a denial; an unsolicited one (the
 * host's re-read of iOS's authorisation at a foreground) only updates the grant, as Android would kill the process
 * on a revocation where Minato answers differently from then on. Plain Java, so tools/framework-host-check.sh runs
 * it on the host's JVM.
 */
public final class PermissionAnswers {
    /** The outcomes of APKRT_EVENT_PERMISSION_RESULT's third argument; the microphone has DENIED and GRANTED_FINE. */
    public static final int DENIED = 0;
    public static final int GRANTED_FINE = 1;
    public static final int GRANTED_COARSE = 2;

    private static final String FINE = "android.permission.ACCESS_FINE_LOCATION";
    private static final String COARSE = "android.permission.ACCESS_COARSE_LOCATION";
    /** Android asks again after one denial and stops asking after the second. */
    private static final int DENIALS_FINAL = 2;

    private int mOutcome = DENIED;
    private int mDenials;

    /**
     * @param group     PermissionPolicy.GROUP_LOCATION; another group is ignored
     * @param outcome   DENIED, GRANTED_FINE or GRANTED_COARSE; an unknown value is a denial
     * @param solicited the result of a request, not a re-read
     */
    public synchronized void answer(int group, int outcome, boolean solicited) {
        if (group != PermissionPolicy.GROUP_LOCATION) {
            return;
        }
        if (outcome == GRANTED_FINE || outcome == GRANTED_COARSE) {
            mOutcome = outcome;
            mDenials = 0;
            return;
        }
        mOutcome = DENIED;
        if (solicited && outcome == DENIED) {
            mDenials++;
        }
    }

    /** Whether the host's answer grants the permission; false for a permission of no host group. */
    public synchronized boolean granted(String permission) {
        if (PermissionPolicy.hostGroup(permission) != PermissionPolicy.GROUP_LOCATION) {
            return false;
        }
        return FINE.equals(permission) ? mOutcome == GRANTED_FINE : mOutcome != DENIED;
    }

    /** Whether a request for the permission goes to the host: it is not granted and was denied less than twice. */
    public synchronized boolean mustAsk(String permission) {
        return PermissionPolicy.hostGroup(permission) == PermissionPolicy.GROUP_LOCATION && !granted(permission)
                && mDenials < DENIALS_FINAL;
    }

    /** Android's shouldShowRequestPermissionRationale: after one denial, not before the first or after the last. */
    public synchronized boolean shouldShowRationale(String permission) {
        return PermissionPolicy.hostGroup(permission) == PermissionPolicy.GROUP_LOCATION && !granted(permission)
                && mDenials == 1;
    }

    /** APKRT_REQUEST_PERMISSION's detail for the location group: 1 when fine location is among them, 2 for coarse only, else 0. */
    public static int locationDetail(Collection<String> permissions) {
        if (permissions.contains(FINE)) {
            return 1;
        }
        return permissions.contains(COARSE) ? 2 : 0;
    }
}
