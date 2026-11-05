// The written part of BluetoothManager (services batches B and E): an adapter that is there and off, as the emulator
// (apk-runner-35) has one [verified: services-b-test: getAdapter() is not null, connected devices need
// BLUETOOTH_CONNECT; bluetooth-gatt-test: the same for the connection state and the GATT server's null callback].
// The iPhone's Bluetooth is not offered to guests, so the adapter never turns on and no GATT server opens.
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
        BluetoothDevice.checkConnect("getConnectedDevices");
        return new java.util.ArrayList<>();
    }

    public int getConnectionState(BluetoothDevice device, int profile) {
        BluetoothDevice.checkConnect("getConnectionState");
        return BluetoothProfile.STATE_DISCONNECTED;
    }

    public java.util.List<BluetoothDevice> getDevicesMatchingConnectionStates(int profile, int[] states) {
        BluetoothDevice.checkConnect("getDevicesMatchingConnectionStates");
        return new java.util.ArrayList<>();
    }

    public BluetoothGattServer openGattServer(Context context, BluetoothGattServerCallback callback) {
        if (context == null || callback == null) {
            throw new IllegalArgumentException("null parameter: " + context + " " + callback);
        }
        BluetoothDevice.checkConnect("openGattServer");
        return null;
    }
}
