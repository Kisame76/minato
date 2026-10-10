package android.service.notification;

import android.content.ComponentName;
import android.net.Uri;
import com.android.minato.services.Unwritten;

/**
 * Placeholder (App, the Unity test APK retest 11): our class under AOSP's
 * hidden name, which only has to load, since a class apps reflect over names it; every body logs once and throws
 * UnsupportedOperationException. Before, a compile-only stub (ADR-0045 gap 1, batch 2): Do Not Disturb's configuration
 * and its rules, which INotificationManager's Stub and Proxy pass and NotificationManager's zen rules read; Minato has
 * no Do Not Disturb, and its notification service grants no policy access.
 */
public class ZenModeConfig implements android.os.Parcelable {
    public ZenModeConfig() {
        throw Unwritten.placeholder("android.service.notification.ZenModeConfig()");
    }

    public static final android.os.Parcelable.Creator<ZenModeConfig> CREATOR = null;

    public void writeToParcel(android.os.Parcel out, int flags) {
        throw Unwritten.placeholder("android.service.notification.ZenModeConfig.writeToParcel(android.os.Parcel,int)");
    }

    public int describeContents() {
        throw Unwritten.placeholder("android.service.notification.ZenModeConfig.describeContents()");
    }

    public static class ZenRule implements android.os.Parcelable {
        public ZenRule() {
            throw Unwritten.placeholder("android.service.notification.ZenModeConfig.ZenRule()");
        }

        public static final android.os.Parcelable.Creator<ZenRule> CREATOR = null;

        public boolean enabled;
        public String name;
        public int zenMode;
        public Uri conditionId;
        public ComponentName component;
        public ComponentName configurationActivity;
        public String id;
        public long creationTime;
        public String pkg;
        public ZenPolicy zenPolicy;

        public void writeToParcel(android.os.Parcel out, int flags) {
            throw Unwritten.placeholder(
                    "android.service.notification.ZenModeConfig.writeToParcel(android.os.Parcel,int)");
        }

        public int describeContents() {
            throw Unwritten.placeholder("android.service.notification.ZenModeConfig.describeContents()");
        }
    }
}
