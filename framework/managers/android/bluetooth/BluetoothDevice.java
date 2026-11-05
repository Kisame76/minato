// The written part of BluetoothDevice (services batches B and E): a remote device an app asks the adapter for by
// address. Its address is what the app gave, its toString hides the first four bytes as Android's does, and equal
// addresses are equal devices. What the stack knows of a device (its name, type, bond state, class and services) is
// behind BLUETOOTH_CONNECT, the SecurityException Android 15 gives an app without it [verified: bluetooth-gatt-test];
// with the permission the adapter is off, so the device is unknown: no name, type unknown, not bonded, and no GATT
// connection can open (connectGatt returns null, as when the stack has no GATT service). Intent's
// prepareToEnterProcess hands the receiving app's attribution to a device extra of the Bluetooth intents; a device
// here has no attribution to keep.
import android.app.ActivityThread;
import android.content.AttributionSource;
import android.content.Context;
import android.os.Handler;

import java.util.UUID;

class BluetoothDevice {
    private static final String CONNECT = "android.permission.BLUETOOTH_CONNECT";

    private final String mAddress;

    /** @hide */
    BluetoothDevice(String address) {
        if (!BluetoothAdapter.checkBluetoothAddress(address)) {
            throw new IllegalArgumentException(address + " is not a valid Bluetooth address");
        }
        mAddress = address;
    }

    /** @hide The calling app's BLUETOOTH_CONNECT, which the stack's service checks on Android. */
    static void checkConnect(String what) {
        check(CONNECT, what);
    }

    /** @hide The calling app's BLUETOOTH_SCAN. */
    static void checkScan(String what) {
        check("android.permission.BLUETOOTH_SCAN", what);
    }

    private static void check(String permission, String what) {
        final android.app.Application app = ActivityThread.currentApplication();
        if (app != null
                && app.checkSelfPermission(permission) == android.content.pm.PackageManager.PERMISSION_GRANTED) {
            return;
        }
        throw new SecurityException(what + ": Need " + permission + " permission");
    }

    /** @hide */
    public void prepareToEnterProcess(AttributionSource attributionSource) {
    }

    public String getAddress() {
        return mAddress;
    }

    /** @hide Object's, with the device's address. */
    public String toString() {
        return "XX:XX:XX:XX:" + mAddress.substring(12);
    }

    /** @hide Object's, with the device's address. */
    public boolean equals(Object o) {
        return o instanceof BluetoothDevice && mAddress.equals(((BluetoothDevice) o).mAddress);
    }

    /** @hide Object's, with the device's address. */
    public int hashCode() {
        return mAddress.hashCode();
    }

    public int describeContents() {
        return 0;
    }

    public String getName() {
        checkConnect("getName");
        return null;
    }

    public String getAlias() {
        checkConnect("getAlias");
        return null;
    }

    public int setAlias(String alias) {
        checkConnect("setAlias");
        return 2;
    }

    public int getType() {
        checkConnect("getType");
        return DEVICE_TYPE_UNKNOWN;
    }

    public int getBondState() {
        checkConnect("getBondState");
        return BOND_NONE;
    }

    public BluetoothClass getBluetoothClass() {
        checkConnect("getBluetoothClass");
        return null;
    }

    public android.os.ParcelUuid[] getUuids() {
        checkConnect("getUuids");
        return null;
    }

    public boolean fetchUuidsWithSdp() {
        checkConnect("fetchUuidsWithSdp");
        return false;
    }

    public boolean createBond() {
        checkConnect("createBond");
        return false;
    }

    public boolean setPin(byte[] pin) {
        checkConnect("setPin");
        return false;
    }

    public BluetoothGatt connectGatt(Context context, boolean autoConnect, BluetoothGattCallback callback) {
        return connectGatt(context, autoConnect, callback, TRANSPORT_AUTO);
    }

    public BluetoothGatt connectGatt(Context context, boolean autoConnect, BluetoothGattCallback callback,
            int transport) {
        return connectGatt(context, autoConnect, callback, transport, PHY_LE_1M_MASK);
    }

    public BluetoothGatt connectGatt(Context context, boolean autoConnect, BluetoothGattCallback callback,
            int transport, int phy) {
        return connectGatt(context, autoConnect, callback, transport, phy, null);
    }

    public BluetoothGatt connectGatt(Context context, boolean autoConnect, BluetoothGattCallback callback,
            int transport, int phy, Handler handler) {
        if (callback == null) {
            throw new NullPointerException("callback is null");
        }
        checkConnect("connectGatt");
        return null;
    }

    public BluetoothSocket createRfcommSocketToServiceRecord(UUID uuid) throws java.io.IOException {
        return new BluetoothSocket(this);
    }

    public BluetoothSocket createInsecureRfcommSocketToServiceRecord(UUID uuid) throws java.io.IOException {
        return new BluetoothSocket(this);
    }
}
