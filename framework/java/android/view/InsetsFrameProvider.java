package android.view;

import com.android.minato.services.Unwritten;

/**
 * Placeholder (the Unity test APK retest 11; replaced by ADR-0058 b) / category 2): our class under AOSP's
 * hidden name, which only has to load, since a class apps reflect over names it; every body logs once and throws
 * UnsupportedOperationException. Before, a compile-only stub (ADR-0039 step c2c3):
 * WindowManager.LayoutParams.providedInsets, which system windows set and the params parcel.
 */
public class InsetsFrameProvider implements android.os.Parcelable {
    public InsetsFrameProvider() {
        throw Unwritten.placeholder("android.view.InsetsFrameProvider()");
    }

    public static final android.os.Parcelable.Creator<InsetsFrameProvider> CREATOR = null;

    public int describeContents() {
        throw Unwritten.placeholder("android.view.InsetsFrameProvider.describeContents()");
    }

    public void writeToParcel(android.os.Parcel out, int flags) {
        throw Unwritten.placeholder("android.view.InsetsFrameProvider.writeToParcel(android.os.Parcel,int)");
    }
}
