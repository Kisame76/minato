// The written part of BluetoothManager (services batch B): an adapter that is there and off, as the emulator
// (apk-runner-35) has one [verified: services-b-test: getAdapter() is not null, connected devices need
// BLUETOOTH_CONNECT]. The iPhone's Bluetooth is not offered to guests, so the adapter never turns on.
import android.content.Context;

class BluetoothManager {
    private final Context mContext;

    /** @hide As the tag's, which SystemServiceRegistry calls with the outer context. */
    public BluetoothManager(Context context) {
        mContext = context;
    }

    public BluetoothAdapter getAdapter() {
        return BluetoothAdapter.getDefaultAdapter();
    }

    public java.util.List<BluetoothDevice> getConnectedDevices(int profile) {
        if (profile != BluetoothProfile.GATT && profile != BluetoothProfile.GATT_SERVER) {
            throw new IllegalArgumentException("Profile not supported: " + profile);
        }
        throw new SecurityException("Need BLUETOOTH_CONNECT permission");
    }
}
