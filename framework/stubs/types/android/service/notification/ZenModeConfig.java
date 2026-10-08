package android.service.notification;

import android.content.ComponentName;
import android.net.Uri;

/**
 * Compile-only stub (ADR-0045 gap 1, batch 2), never in framework.jar: Do Not Disturb's configuration and its rules, which
 * INotificationManager's Stub and Proxy pass and NotificationManager's zen rules read; Minato has no Do Not Disturb,
 * and its notification service grants no policy access.
 */
public class ZenModeConfig implements android.os.Parcelable {
    public static final android.os.Parcelable.Creator<ZenModeConfig> CREATOR = null;

    public void writeToParcel(android.os.Parcel out, int flags) {
        throw new RuntimeException("stub");
    }

    public int describeContents() {
        throw new RuntimeException("stub");
    }

    public static class ZenRule implements android.os.Parcelable {
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
            throw new RuntimeException("stub");
        }

        public int describeContents() {
            throw new RuntimeException("stub");
        }
    }
}
