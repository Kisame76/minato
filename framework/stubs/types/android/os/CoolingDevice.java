package android.os;

/**
 * Compile-only stub (ADR-0045), never in framework.jar: the thermal HAL's cooling device, which
 * IThermalService's Stub and Proxy parcel; the local thermal service lists none.
 */
public final class CoolingDevice implements android.os.Parcelable {
    public static final android.os.Parcelable.Creator<CoolingDevice> CREATOR = null;

    public void writeToParcel(android.os.Parcel out, int flags) {
        throw new RuntimeException("stub");
    }

    public int describeContents() {
        throw new RuntimeException("stub");
    }
}
