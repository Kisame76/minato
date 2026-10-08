package android.os;

/**
 * Compile-only stub (ADR-0045 gap 1, batch 1), never in framework.jar: the battery saver's policy, which
 * PowerManager's full and adaptive policy system APIs and IPowerManager's Stub and Proxy take; they need
 * DEVICE_POWER or POWER_SAVER, which no app holds.
 */
public final class BatterySaverPolicyConfig implements android.os.Parcelable {
    public static final android.os.Parcelable.Creator<BatterySaverPolicyConfig> CREATOR = null;

    public void writeToParcel(android.os.Parcel out, int flags) {
        throw new RuntimeException("stub");
    }

    public int describeContents() {
        throw new RuntimeException("stub");
    }
}
