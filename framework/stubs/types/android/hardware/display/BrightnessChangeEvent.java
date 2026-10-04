package android.hardware.display;

import android.os.Parcel;
import android.os.Parcelable;

/**
 * Compile-only stub (ADR-0041 a)), never in framework.jar: DisplayManagerGlobal names it as the brightness events'
 * element type, for a call apps on Minato's path do not make.
 */
public final class BrightnessChangeEvent implements Parcelable {
    @Override
    public int describeContents() {
        throw new RuntimeException("stub");
    }

    @Override
    public void writeToParcel(Parcel dest, int flags) {
        throw new RuntimeException("stub");
    }
}
