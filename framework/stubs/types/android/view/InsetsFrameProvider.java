package android.view;

/**
 * Compile-only stub (ADR-0039 step c2c3), never in framework.jar: WindowManager.LayoutParams.providedInsets, which
 * system windows set and the params parcel.
 */
public class InsetsFrameProvider implements android.os.Parcelable {
    public static final android.os.Parcelable.Creator<InsetsFrameProvider> CREATOR = null;

    public int describeContents() {
        throw new RuntimeException("stub");
    }

    public void writeToParcel(android.os.Parcel out, int flags) {
        throw new RuntimeException("stub");
    }
}
