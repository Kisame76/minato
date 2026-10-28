package android.os;

import com.android.minato.services.Unwritten;

/**
 * Placeholder (App, the gap list of 2026-10-11): our class under AOSP's hidden name, which only has to load, since
 * VibrationEffect and its segments name it and apps reflect over VibrationEffect; every body logs once and throws
 * UnsupportedOperationException. Before, a compile-only stub (ADR-0045 gap 1, batch 2): a vibrator's capabilities,
 * against which a vibration effect checks itself before the vibrator service plays it; Minato's vibrator comes with
 * the vibrator's step. It is Parcelable only for IVibratorManagerService's proxy and stub (gap 1, batch 3), which a
 * local service never uses: the vibrator manager service has no vibrator and gives no info.
 */
public class VibratorInfo implements Parcelable {
    public static final Creator<VibratorInfo> CREATOR = new Creator<VibratorInfo>() {
        @Override
        public VibratorInfo createFromParcel(Parcel in) {
            throw Unwritten.placeholder("android.os.VibratorInfo.CREATOR.createFromParcel(android.os.Parcel)");
        }

        @Override
        public VibratorInfo[] newArray(int size) {
            throw Unwritten.placeholder("android.os.VibratorInfo.CREATOR.newArray(int)");
        }
    };

    private VibratorInfo() {
        throw Unwritten.placeholder("android.os.VibratorInfo()");
    }

    public boolean hasAmplitudeControl() {
        throw Unwritten.placeholder("android.os.VibratorInfo.hasAmplitudeControl()");
    }

    public boolean hasFrequencyControl() {
        throw Unwritten.placeholder("android.os.VibratorInfo.hasFrequencyControl()");
    }

    public int isEffectSupported(int effectId) {
        throw Unwritten.placeholder("android.os.VibratorInfo.isEffectSupported(int)");
    }

    public boolean isPrimitiveSupported(int primitiveId) {
        throw Unwritten.placeholder("android.os.VibratorInfo.isPrimitiveSupported(int)");
    }

    @Override
    public int describeContents() {
        throw Unwritten.placeholder("android.os.VibratorInfo.describeContents()");
    }

    @Override
    public void writeToParcel(Parcel dest, int flags) {
        throw Unwritten.placeholder("android.os.VibratorInfo.writeToParcel(android.os.Parcel,int)");
    }
}
