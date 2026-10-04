package android.content.pm;

import android.os.Parcel;
import android.os.Parcelable;
import java.util.List;

/**
 * Compile-only stub (ADR-0041 a)), never in framework.jar: IDisplayManager's AIDL code and DisplayManagerGlobal name it, for
 * calls apps on Minato's path do not make.
 */
public class ParceledListSlice<T extends Parcelable> implements Parcelable {
    public static final Parcelable.Creator<ParceledListSlice> CREATOR = null;

    public List<T> getList() {
        throw new RuntimeException("stub");
    }

    @Override
    public int describeContents() {
        throw new RuntimeException("stub");
    }

    @Override
    public void writeToParcel(Parcel dest, int flags) {
        throw new RuntimeException("stub");
    }
}
