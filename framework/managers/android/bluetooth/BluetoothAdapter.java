// The written part of BluetoothAdapter (services batches B and E): the one adapter, off. Its state is STATE_OFF and it
// is not enabled, since the iPhone's Bluetooth is not offered to guests (the emulator's adapter is on, so the lines of
// bluetooth-gatt-test that depend on that are only shown). An off adapter has no scanner, no advertiser and none of the
// LE features. What Android's service checks for the calling app is checked here as it is: BLUETOOTH_CONNECT for the
// name, the bonded devices, enable, disable and the server sockets, and BLUETOOTH_SCAN for discovery and the scan mode
// [verified: bluetooth-gatt-test for the exceptions of an app that holds neither]; an app that holds them gets
// what an adapter that is off answers.
import android.bluetooth.le.BluetoothLeAdvertiser;
import android.bluetooth.le.BluetoothLeScanner;

import java.util.HashSet;
import java.util.Set;
import java.util.UUID;
import java.util.regex.Pattern;

class BluetoothAdapter {
    private static final BluetoothAdapter sAdapter = new BluetoothAdapter();
    private static final Pattern ADDRESS = Pattern.compile("([0-9A-F]{2}:){5}[0-9A-F]{2}");

    /** @hide */
    BluetoothAdapter() {
    }

    public static BluetoothAdapter getDefaultAdapter() {
        return sAdapter;
    }

    public static boolean checkBluetoothAddress(String address) {
        return address != null && ADDRESS.matcher(address).matches();
    }

    public int getState() {
        return STATE_OFF;
    }

    public boolean isEnabled() {
        return false;
    }

    public BluetoothDevice getRemoteDevice(String address) {
        return new BluetoothDevice(address);
    }

    public BluetoothDevice getRemoteDevice(byte[] address) {
        if (address == null || address.length != 6) {
            throw new IllegalArgumentException("Bluetooth address must have 6 bytes");
        }
        return new BluetoothDevice(String.format("%02X:%02X:%02X:%02X:%02X:%02X", address[0], address[1], address[2],
                address[3], address[4], address[5]));
    }

    public BluetoothDevice getRemoteLeDevice(String address, int addressType) {
        return new BluetoothDevice(address);
    }

    public BluetoothLeScanner getBluetoothLeScanner() {
        return null;
    }

    public BluetoothLeAdvertiser getBluetoothLeAdvertiser() {
        return null;
    }

    public String getAddress() {
        BluetoothDevice.checkConnect("getAddress");
        return null;
    }

    public String getName() {
        BluetoothDevice.checkConnect("getName");
        return null;
    }

    public boolean setName(String name) {
        BluetoothDevice.checkConnect("setName");
        return false;
    }

    public Set<BluetoothDevice> getBondedDevices() {
        BluetoothDevice.checkConnect("getBondedDevices");
        return new HashSet<>();
    }

    public int getProfileConnectionState(int profile) {
        BluetoothDevice.checkConnect("getProfileConnectionState");
        return BluetoothProfile.STATE_DISCONNECTED;
    }

    public int getMaxConnectedAudioDevices() {
        BluetoothDevice.checkConnect("getMaxConnectedAudioDevices");
        return 1;
    }

    public boolean enable() {
        BluetoothDevice.checkConnect("enable");
        return false;
    }

    public boolean disable() {
        BluetoothDevice.checkConnect("disable");
        return false;
    }

    public int getScanMode() {
        BluetoothDevice.checkScan("getScanMode");
        return SCAN_MODE_NONE;
    }

    public boolean isDiscovering() {
        BluetoothDevice.checkScan("isDiscovering");
        return false;
    }

    public boolean startDiscovery() {
        BluetoothDevice.checkScan("startDiscovery");
        return false;
    }

    public boolean cancelDiscovery() {
        BluetoothDevice.checkScan("cancelDiscovery");
        return false;
    }

    public java.time.Duration getDiscoverableTimeout() {
        BluetoothDevice.checkScan("getDiscoverableTimeout");
        return null;
    }

    public BluetoothServerSocket listenUsingRfcommWithServiceRecord(String name, UUID uuid)
            throws java.io.IOException {
        BluetoothDevice.checkConnect("listenUsingRfcommWithServiceRecord");
        throw new java.io.IOException("Bluetooth is off");
    }

    public BluetoothServerSocket listenUsingInsecureRfcommWithServiceRecord(String name, UUID uuid)
            throws java.io.IOException {
        BluetoothDevice.checkConnect("listenUsingInsecureRfcommWithServiceRecord");
        throw new java.io.IOException("Bluetooth is off");
    }

    public boolean isMultipleAdvertisementSupported() {
        return false;
    }

    public boolean isLe2MPhySupported() {
        return false;
    }

    public boolean isLeCodedPhySupported() {
        return false;
    }

    public boolean isLeExtendedAdvertisingSupported() {
        return false;
    }

    public boolean isLePeriodicAdvertisingSupported() {
        return false;
    }

    public boolean isOffloadedFilteringSupported() {
        return false;
    }

    public boolean isOffloadedScanBatchingSupported() {
        return false;
    }
}
