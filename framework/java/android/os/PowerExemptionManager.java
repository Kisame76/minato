/*
 * Our PowerExemptionManager, under AOSP's name (ADR-0045 decision 3): the
 * power exemption service's manager, which apex/jobscheduler registers on Android and PowerManager asks for
 * isIgnoringBatteryOptimizations. No app is on the power allow list of a phone it was installed on, so the answer
 * is false, as Android 15's is for the test app [verified: services-test on apk-runner-35-bare]; Minato has no
 * battery optimizations to be exempt from, as iOS decides what a backgrounded app may do. Only the members
 * framework.jar's classes call; its system API comes when an app needs it.
 */
package android.os;

import android.content.Context;

public final class PowerExemptionManager {
    private final Context mContext;

    /** @hide SystemServiceRegistry's, with the context it builds the manager for. */
    public PowerExemptionManager(Context context) {
        mContext = context;
    }

    /** @hide PowerManager.isIgnoringBatteryOptimizations': whether the package is on the power allow list. */
    public boolean isAllowListed(String packageName, boolean includingIdle) {
        return false;
    }
}
