package android.app;

/**
 * Compile-only stub (ADR-0045 gap 1, batch 2), never in framework.jar: the notification history, which INotificationManager's Stub and
 * Proxy pass and only the system reads.
 */
public final class NotificationHistory implements android.os.Parcelable {
    public static final android.os.Parcelable.Creator<NotificationHistory> CREATOR = null;

    public void writeToParcel(android.os.Parcel out, int flags) {
        throw new RuntimeException("stub");
    }

    public int describeContents() {
        throw new RuntimeException("stub");
    }
}
