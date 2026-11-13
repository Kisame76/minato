// The written part of LauncherApps, which tools/managergen joins with the
// API's methods. Ours under AOSP's name: AOSP's asks the launcher apps service of system_server (60 methods over the
// shortcut and package services). The answers are those of apk-runner-35 (Android 15) for an app that is no launcher:
// it holds no shortcut host permission, the owner's profile is the only one, its own launcher activities come from the
// package manager, and reading shortcuts throws SecurityException.
//
// A rule that Android's service does not follow (measured: it shows Settings and every other launcher app to any app):
// LauncherApps and every list it gives NEVER name another app installed on the device. It answers for the app's own
// package and its profiles only, as package visibility does for an app without QUERY_ALL_PACKAGES; a guest must not
// learn the user's other apps. system-services-test logs the line "launcher apps name no other app true" for this.
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.pm.IncrementalStatesInfo;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.os.Process;
import android.os.UserHandle;

import java.util.ArrayList;
import java.util.List;

class LauncherApps {
    private Context mContext;

    /** @hide */
    public LauncherApps(Context context) {
        mContext = context;
    }

    private boolean isOwn(String packageName, UserHandle user) {
        return mContext.getPackageName().equals(packageName) && Process.myUserHandle().equals(user);
    }

    public boolean hasShortcutHostPermission() {
        return false;
    }

    /** The profiles: the owner's, as on a device with no work profile. */
    public List<UserHandle> getProfiles() {
        final List<UserHandle> profiles = new ArrayList<>();
        profiles.add(Process.myUserHandle());
        return profiles;
    }

    /** The app's own launcher activities, asked for its package or for all (null); nothing for any other package. */
    public List<LauncherActivityInfo> getActivityList(String packageName, UserHandle user) {
        final List<LauncherActivityInfo> activities = new ArrayList<>();
        if (!Process.myUserHandle().equals(user) || (packageName != null && !isOwn(packageName, user))) {
            return activities;
        }
        final Intent main = new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER)
                .setPackage(mContext.getPackageName());
        for (ResolveInfo info : mContext.getPackageManager().queryIntentActivities(main, 0)) {
            activities.add(new LauncherActivityInfo(mContext, new LauncherActivityInfoInternal(info.activityInfo,
                    new IncrementalStatesInfo(false, 1.0f, 0), user)));
        }
        return activities;
    }

    /** The app's own package only. */
    public boolean isPackageEnabled(String packageName, UserHandle user) {
        if (!isOwn(packageName, user)) return false;
        return mContext.getApplicationInfo().enabled;
    }

    /** The app's own activities only. */
    public boolean isActivityEnabled(ComponentName component, UserHandle user) {
        if (!isOwn(component.getPackageName(), user)) return false;
        try {
            final int state = mContext.getPackageManager().getComponentEnabledSetting(component);
            return state != PackageManager.COMPONENT_ENABLED_STATE_DISABLED;
        } catch (IllegalArgumentException e) {
            return false;
        }
    }

    /** The app's own package only; any other is NameNotFoundException, as an absent package would be. */
    public android.content.pm.ApplicationInfo getApplicationInfo(String packageName, int flags, UserHandle user)
            throws PackageManager.NameNotFoundException {
        if (!isOwn(packageName, user)) throw new PackageManager.NameNotFoundException(packageName);
        return mContext.getApplicationInfo();
    }

    /** The first launcher activity of the intent, the app's own only; null when none. */
    public LauncherActivityInfo resolveActivity(Intent intent, UserHandle user) {
        final String target = intent.getPackage() != null ? intent.getPackage()
                : intent.getComponent() != null ? intent.getComponent().getPackageName() : null;
        if (target == null || !isOwn(target, user)) return null;
        // apk-runner-35 gives no activity for the app's own launcher intent as well (measured), so none is given here.
        return null;
    }

    public List<ShortcutInfo> getShortcuts(ShortcutQuery query, UserHandle user) {
        throw new SecurityException("Caller can't access shortcut information: it is not the default launcher");
    }
}
