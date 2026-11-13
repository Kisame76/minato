package android.os;

/**
 * Compile-only stub (ADR-0045), never in framework.jar: the thermal HAL's temperature, which
 * IThermalService's Stub and Proxy parcel and PowerManager's THERMAL_STATUS_ constants take their values from
 * (ThrottlingSeverity's at android-15.0.0_r1); the local thermal service lists no temperatures.
 */
public final class Temperature implements android.os.Parcelable {
    public static final int THROTTLING_NONE = 0;
    public static final int THROTTLING_LIGHT = 1;
    public static final int THROTTLING_MODERATE = 2;
    public static final int THROTTLING_SEVERE = 3;
    public static final int THROTTLING_CRITICAL = 4;
    public static final int THROTTLING_EMERGENCY = 5;
    public static final int THROTTLING_SHUTDOWN = 6;

    public static final android.os.Parcelable.Creator<Temperature> CREATOR = null;

    public void writeToParcel(android.os.Parcel out, int flags) {
        throw new RuntimeException("stub");
    }

    public int describeContents() {
        throw new RuntimeException("stub");
    }
}
