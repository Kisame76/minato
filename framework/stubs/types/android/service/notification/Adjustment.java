package android.service.notification;

/**
 * Compile-only stub (ADR-0045 gap 1, batch 2), never in framework.jar: an assistant's adjustment of a notification, which INotificationManager's Stub and Proxy pass and
 * NotificationManager's assistant calls name.
 */
public final class Adjustment implements android.os.Parcelable {
    public static final android.os.Parcelable.Creator<Adjustment> CREATOR = null;

    /** NotificationManager's allowed adjustments name it. */
    @java.lang.annotation.Retention(java.lang.annotation.RetentionPolicy.SOURCE)
    public @interface Keys {
    }

    public void writeToParcel(android.os.Parcel out, int flags) {
        throw new RuntimeException("stub");
    }

    public int describeContents() {
        throw new RuntimeException("stub");
    }
}
