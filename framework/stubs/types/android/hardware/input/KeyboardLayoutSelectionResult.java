package android.hardware.input;

/**
 * Compile-only stub (ADR-0045, the input service), never in framework.jar: a keyboard layout's selection, which only
 * IInputManager's Stub and Proxy name.
 */
public final class KeyboardLayoutSelectionResult implements android.os.Parcelable {
    public static final android.os.Parcelable.Creator<KeyboardLayoutSelectionResult> CREATOR = null;

    public int describeContents() {
        throw new RuntimeException("stub");
    }

    public void writeToParcel(android.os.Parcel dest, int flags) {
        throw new RuntimeException("stub");
    }
}
