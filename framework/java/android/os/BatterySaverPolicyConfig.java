package android.os;

import com.android.minato.services.Unwritten;

/**
 * Placeholder (the Unity test APK retest 11): our class under AOSP's
 * hidden name, which only has to load, since a class apps reflect over names it; every body logs once and throws
 * UnsupportedOperationException. Before, a compile-only stub (ADR-0045 gap 1, batch 1): the battery saver's policy,
 * which PowerManager's full and adaptive policy system APIs and IPowerManager's Stub and Proxy take; they need
 * DEVICE_POWER or POWER_SAVER, which no app holds.
 */
public final class BatterySaverPolicyConfig implements android.os.Parcelable {
    public BatterySaverPolicyConfig() {
        throw Unwritten.placeholder("android.os.BatterySaverPolicyConfig()");
    }

    public static final android.os.Parcelable.Creator<BatterySaverPolicyConfig> CREATOR = null;

    public void writeToParcel(android.os.Parcel out, int flags) {
        throw Unwritten.placeholder("android.os.BatterySaverPolicyConfig.writeToParcel(android.os.Parcel,int)");
    }

    public int describeContents() {
        throw Unwritten.placeholder("android.os.BatterySaverPolicyConfig.describeContents()");
    }
}
