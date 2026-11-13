package android.os.connectivity;

/**
 * Compile-only stub (the battery), never in framework.jar: a type of the battery
 * stats' interface (IBatteryStats), which only its Stub and Proxy parcel; the local battery stats service never
 * runs them.
 */
public final class WifiBatteryStats implements android.os.Parcelable {
    public static final android.os.Parcelable.Creator<WifiBatteryStats> CREATOR = null;

    public void writeToParcel(android.os.Parcel out, int flags) {
        throw new RuntimeException("stub");
    }

    public int describeContents() {
        throw new RuntimeException("stub");
    }
}
