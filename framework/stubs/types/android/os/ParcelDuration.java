package android.os;

/**
 * Compile-only stub (ADR-0045), never in framework.jar: a Duration in a parcel, which PowerManager's
 * battery discharge prediction reads; the local power service gives no prediction, so PowerManager answers null.
 */
public final class ParcelDuration implements android.os.Parcelable {
    public static final android.os.Parcelable.Creator<ParcelDuration> CREATOR = null;

    public ParcelDuration(java.time.Duration duration) {
        throw new RuntimeException("stub");
    }

    public java.time.Duration getDuration() {
        throw new RuntimeException("stub");
    }

    public void writeToParcel(android.os.Parcel out, int flags) {
        throw new RuntimeException("stub");
    }

    public int describeContents() {
        throw new RuntimeException("stub");
    }
}
