// The written part of BluetoothDevice (services batch B): the device an adapter or an intent gives. Intent's
// prepareToEnterProcess hands the receiving app's attribution to a device extra of the Bluetooth intents; a device here
// has no attribution to keep.
import android.content.AttributionSource;

class BluetoothDevice {
    /** @hide */
    public void prepareToEnterProcess(AttributionSource attributionSource) {
    }
}
