package android.bluetooth;

/**
 * Compile-only stub (ADR-0039 step c2c1), never in framework.jar: the Bluetooth module's (not fetched);
 * Intent.prepareToEnterProcess fixes the attribution of a Bluetooth intent's device extra.
 */
public final class BluetoothDevice implements android.os.Parcelable {
    public static final String EXTRA_DEVICE = "android.bluetooth.device.extra.DEVICE";

    public void prepareToEnterProcess(android.content.AttributionSource attributionSource) {
        throw new RuntimeException("stub");
    }

    public void writeToParcel(android.os.Parcel out, int flags) {
        throw new RuntimeException("stub");
    }

    public int describeContents() {
        throw new RuntimeException("stub");
    }
}
