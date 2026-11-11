package com.android.internal.minato.vpn;

import android.app.Activity;
import android.content.ComponentName;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.os.Bundle;

/**
 * The activity that stands in for Android's VPN consent dialog, com.android.vpndialogs/.ConfirmDialog, which
 * VpnService.prepare hands to an app to start for a result. iOS gives Minato no VPN, so the user's answer is the one a
 * phone gives when the user says no (its "Cancel" button): RESULT_CANCELED with no data [verified: vpn-test on
 * apk-runner-35; the dialog ignores BACK, and the caller is only paused under it, as it is under a translucent
 * theme]. It has no window of its own, and asks nobody: nothing appears on the screen.
 */
public final class VpnConsentActivity extends Activity {
    private static final String CLASS = VpnConsentActivity.class.getName();
    private static final String DIALOG_PACKAGE = "com.android.vpndialogs";
    private static final String DIALOG_CLASS = "com.android.vpndialogs.ConfirmDialog";

    /** Whether `component` names Android's consent dialog (VpnConfig names it with the short class name). */
    public static boolean handles(ComponentName component) {
        return DIALOG_PACKAGE.equals(component.getPackageName())
            && (DIALOG_CLASS.equals(component.getClassName()) || ".ConfirmDialog".equals(component.getClassName()));
    }

    /** The activity of the framework class, in the app's package as an activity of the app is. */
    public static ActivityInfo activityInfo(ApplicationInfo app) {
        final ActivityInfo info = new ActivityInfo();
        info.packageName = app.packageName;
        info.name = CLASS;
        info.applicationInfo = app;
        info.processName = app.processName;
        info.exported = false;
        info.launchMode = ActivityInfo.LAUNCH_MULTIPLE;
        info.configChanges = -1;
        info.theme = com.android.internal.R.style.Theme_Translucent_NoTitleBar;
        return info;
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setResult(RESULT_CANCELED);
        finish();
    }
}
