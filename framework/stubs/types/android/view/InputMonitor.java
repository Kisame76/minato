package android.view;

/**
 * Compile-only stub (ADR-0045, the input service), never in framework.jar: InputManager.monitorGestureInput's monitor,
 * which needs MONITOR_INPUT, a signature permission.
 */
public final class InputMonitor implements android.os.Parcelable {
    public static final android.os.Parcelable.Creator<InputMonitor> CREATOR = null;

    public int describeContents() {
        throw new RuntimeException("stub");
    }

    public void writeToParcel(android.os.Parcel dest, int flags) {
        throw new RuntimeException("stub");
    }
}
