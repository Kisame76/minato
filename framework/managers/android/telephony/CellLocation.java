// The written part of CellLocation (F4d): the hidden members the GSM and CDMA cell locations implement, and
// getEmpty and newFromBundle, which AOSP answers by the phone type [verified: CellLocation.java at android-15.0.0_r1,
// frameworks/base/telephony/java/android/telephony]. Minato's phone type is PHONE_TYPE_NONE (TelephonyManager), for
// which both give null. requestLocationUpdate is AOSP's with no telephony service: its phone is null, and the method
// does nothing.
import android.os.Bundle;

abstract class CellLocation {
    /** @hide */
    public static CellLocation newFromBundle(Bundle bundle) {
        return null;
    }

    /** @hide */
    public abstract void fillInNotifierBundle(Bundle bundle);

    /** @hide */
    public abstract boolean isEmpty();

    /** @hide Invalidates this object: the location area code and the cell id become -1. */
    public abstract void setStateInvalid();

    public static CellLocation getEmpty() {
        return null;
    }

    public static void requestLocationUpdate() {
    }
}
