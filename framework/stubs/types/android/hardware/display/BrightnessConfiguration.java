package android.hardware.display;

import android.os.Parcel;
import android.os.Parcelable;

/**
 * Compile-only stub (ADR-0041 a)), never in framework.jar: IDisplayManager's AIDL code and DisplayManagerGlobal name it, for calls of the display service that apps on
 * Minato's path do not make.
 */
public final class BrightnessConfiguration implements Parcelable {
    public static final Parcelable.Creator<BrightnessConfiguration> CREATOR = null;

    @Override
    public int describeContents() {
        throw new RuntimeException("stub");
    }

    @Override
    public void writeToParcel(Parcel dest, int flags) {
        throw new RuntimeException("stub");
    }
}
