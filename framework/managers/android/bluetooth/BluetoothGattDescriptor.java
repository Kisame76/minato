// The written part of BluetoothGattDescriptor (services batch E): a descriptor of a GATT characteristic, AOSP's data
// holder with a UUID, permissions, the characteristic that owns it and a value [verified: bluetooth-gatt-test].
import java.util.UUID;

class BluetoothGattDescriptor {
    public static final byte[] ENABLE_NOTIFICATION_VALUE = {0x01, 0x00};
    public static final byte[] ENABLE_INDICATION_VALUE = {0x02, 0x00};
    public static final byte[] DISABLE_NOTIFICATION_VALUE = {0x00, 0x00};

    private final UUID mUuid;
    private int mInstance;
    private final int mPermissions;
    private BluetoothGattCharacteristic mCharacteristic;
    private byte[] mValue;

    public BluetoothGattDescriptor(UUID uuid, int permissions) {
        mUuid = uuid;
        mPermissions = permissions;
    }

    /** @hide */
    void setCharacteristic(BluetoothGattCharacteristic characteristic) {
        mCharacteristic = characteristic;
    }

    public int describeContents() {
        return 0;
    }

    public void writeToParcel(android.os.Parcel out, int flags) {
        out.writeParcelable(new android.os.ParcelUuid(mUuid), 0);
        out.writeInt(mInstance);
        out.writeInt(mPermissions);
    }

    public BluetoothGattCharacteristic getCharacteristic() {
        return mCharacteristic;
    }

    public UUID getUuid() {
        return mUuid;
    }

    public int getPermissions() {
        return mPermissions;
    }

    public byte[] getValue() {
        return mValue;
    }

    public boolean setValue(byte[] value) {
        mValue = value;
        return true;
    }
}
