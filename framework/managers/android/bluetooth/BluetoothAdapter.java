// The written part of BluetoothAdapter (services batch B): the one adapter, off. Its state is STATE_OFF and it is not
// enabled; what the generated class gives for the rest (no name, no devices, no scanner) is what an off adapter
// answers.
class BluetoothAdapter {
    private static final BluetoothAdapter sAdapter = new BluetoothAdapter();

    /** @hide */
    BluetoothAdapter() {
    }

    public static BluetoothAdapter getDefaultAdapter() {
        return sAdapter;
    }

    public int getState() {
        return STATE_OFF;
    }

    public boolean isEnabled() {
        return false;
    }
}
