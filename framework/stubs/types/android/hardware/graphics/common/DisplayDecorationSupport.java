package android.hardware.graphics.common;

import android.os.Parcel;
import android.os.Parcelable;

/**
 * Compile-only stub (ADR-0041 a)), never in framework.jar: IDisplayManager's AIDL code, Display and DisplayManagerGlobal name it, for a
 * call apps on Minato's path do not make.
 */
public final class DisplayDecorationSupport implements Parcelable {
    public static final Parcelable.Creator<DisplayDecorationSupport> CREATOR = null;

    @Override
    public int describeContents() {
        throw new RuntimeException("stub");
    }

    @Override
    public void writeToParcel(Parcel dest, int flags) {
        throw new RuntimeException("stub");
    }
}
