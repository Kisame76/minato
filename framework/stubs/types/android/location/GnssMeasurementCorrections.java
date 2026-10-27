package android.location;

import android.os.Parcel;
import android.os.Parcelable;

/**
 * Compile-only stub (ADR-0063), never in framework.jar: the hidden parcelable of ILocationManager's
 * injectGnssMeasurementCorrections, which Minato does not offer (a phone's GNSS measurement corrections: no raw
 * GNSS). It stands for AOSP's class, whose sources import the GNSS HAL's AIDL types, which are not fetched.
 */
public final class GnssMeasurementCorrections implements Parcelable {
    public static final Creator<GnssMeasurementCorrections> CREATOR = null;

    @Override
    public int describeContents() {
        return 0;
    }

    @Override
    public void writeToParcel(Parcel parcel, int flags) {
    }
}
