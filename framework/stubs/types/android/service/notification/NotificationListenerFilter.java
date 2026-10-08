package android.service.notification;

/**
 * Compile-only stub (ADR-0045 gap 1, batch 2), never in framework.jar: a notification listener's filter, which INotificationManager's Stub and Proxy pass.
 */
public final class NotificationListenerFilter implements android.os.Parcelable {
    public static final android.os.Parcelable.Creator<NotificationListenerFilter> CREATOR = null;

    public void writeToParcel(android.os.Parcel out, int flags) {
        throw new RuntimeException("stub");
    }

    public int describeContents() {
        throw new RuntimeException("stub");
    }
}
