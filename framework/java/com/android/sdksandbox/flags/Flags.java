/*
 * Our aconfig flags of AdServices' SDK sandbox (ADR-0039 step c2c3), whose declarations are not among the fetched
 * sources: the one Activity.attach reads. Its value is build_release's for Android 15 (ap3a,
 * sandbox_activity_sdk_based_context_flag_values.textproto): ENABLED, so attach sets the activity's intent before
 * attachBaseContext, as on Android 15.
 */
package com.android.sdksandbox.flags;

/** @hide */
public final class Flags {
    private Flags() {
    }

    public static boolean sandboxActivitySdkBasedContext() {
        return true;
    }
}
