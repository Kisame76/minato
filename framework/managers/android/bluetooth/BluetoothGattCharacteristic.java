// The written part of BluetoothGattCharacteristic (services batch E): a GATT characteristic an app builds or reads. It
// is AOSP's data holder: a UUID, properties, permissions, a write type that is "no response" only for a characteristic
// that has that property, a value that the typed setters write little endian and the typed getters read back, and its
// descriptors [verified: bluetooth-gatt-test].
import java.util.ArrayList;
import java.util.List;
import java.util.UUID;

class BluetoothGattCharacteristic {
    private final UUID mUuid;
    private int mInstance;
    private final int mProperties;
    private final int mPermissions;
    private int mKeySize = 16;
    private int mWriteType;
    private BluetoothGattService mService;
    private byte[] mValue;
    private final List<BluetoothGattDescriptor> mDescriptors = new ArrayList<>();

    public BluetoothGattCharacteristic(UUID uuid, int properties, int permissions) {
        mUuid = uuid;
        mProperties = properties;
        mPermissions = permissions;
        mWriteType = (properties & PROPERTY_WRITE_NO_RESPONSE) != 0 ? WRITE_TYPE_NO_RESPONSE : WRITE_TYPE_DEFAULT;
    }

    /** @hide */
    void setService(BluetoothGattService service) {
        mService = service;
    }

    public boolean addDescriptor(BluetoothGattDescriptor descriptor) {
        mDescriptors.add(descriptor);
        descriptor.setCharacteristic(this);
        return true;
    }

    public int describeContents() {
        return 0;
    }

    public void writeToParcel(android.os.Parcel out, int flags) {
        out.writeParcelable(new android.os.ParcelUuid(mUuid), 0);
        out.writeInt(mInstance);
        out.writeInt(mProperties);
        out.writeInt(mPermissions);
    }

    public BluetoothGattService getService() {
        return mService;
    }

    public UUID getUuid() {
        return mUuid;
    }

    public int getInstanceId() {
        return mInstance;
    }

    /** @hide */
    public void setInstanceId(int instanceId) {
        mInstance = instanceId;
    }

    public int getProperties() {
        return mProperties;
    }

    public int getPermissions() {
        return mPermissions;
    }

    public int getWriteType() {
        return mWriteType;
    }

    public void setWriteType(int writeType) {
        mWriteType = writeType;
    }

    /** @hide */
    public void setKeySize(int keySize) {
        mKeySize = keySize;
    }

    public List<BluetoothGattDescriptor> getDescriptors() {
        return mDescriptors;
    }

    public BluetoothGattDescriptor getDescriptor(UUID uuid) {
        for (BluetoothGattDescriptor descriptor : mDescriptors) {
            if (descriptor.getUuid().equals(uuid)) {
                return descriptor;
            }
        }
        return null;
    }

    public byte[] getValue() {
        return mValue;
    }

    private int getTypeLen(int formatType) {
        return formatType & 0xF;
    }

    private int unsignedByteToInt(byte b) {
        return b & 0xFF;
    }

    private int unsignedBytesToInt(byte b0, byte b1) {
        return unsignedByteToInt(b0) + (unsignedByteToInt(b1) << 8);
    }

    private int unsignedBytesToInt(byte b0, byte b1, byte b2, byte b3) {
        return unsignedByteToInt(b0) + (unsignedByteToInt(b1) << 8) + (unsignedByteToInt(b2) << 16)
                + (unsignedByteToInt(b3) << 24);
    }

    private float bytesToFloat(byte b0, byte b1) {
        int mantissa = unsignedToSigned(unsignedByteToInt(b0) + ((unsignedByteToInt(b1) & 0x0F) << 8), 12);
        int exponent = unsignedToSigned(unsignedByteToInt(b1) >> 4, 4);
        return (float) (mantissa * Math.pow(10, exponent));
    }

    private float bytesToFloat(byte b0, byte b1, byte b2, byte b3) {
        int mantissa = unsignedToSigned(unsignedByteToInt(b0) + (unsignedByteToInt(b1) << 8)
                + (unsignedByteToInt(b2) << 16), 24);
        return (float) (mantissa * Math.pow(10, b3));
    }

    private int unsignedToSigned(int unsigned, int size) {
        if ((unsigned & (1 << size - 1)) != 0) {
            unsigned = -1 * ((1 << size - 1) - (unsigned & ((1 << size - 1) - 1)));
        }
        return unsigned;
    }

    private int intToSignedBits(int i, int size) {
        if (i < 0) {
            i = (1 << size - 1) + (i & ((1 << size - 1) - 1));
        }
        return i;
    }

    public Integer getIntValue(int formatType, int offset) {
        if ((offset + getTypeLen(formatType)) > mValue.length) return null;
        switch (formatType) {
            case FORMAT_UINT8:
                return unsignedByteToInt(mValue[offset]);
            case FORMAT_UINT16:
                return unsignedBytesToInt(mValue[offset], mValue[offset + 1]);
            case FORMAT_UINT32:
                return unsignedBytesToInt(mValue[offset], mValue[offset + 1], mValue[offset + 2], mValue[offset + 3]);
            case FORMAT_SINT8:
                return unsignedToSigned(unsignedByteToInt(mValue[offset]), 8);
            case FORMAT_SINT16:
                return unsignedToSigned(unsignedBytesToInt(mValue[offset], mValue[offset + 1]), 16);
            case FORMAT_SINT32:
                return unsignedToSigned(unsignedBytesToInt(mValue[offset], mValue[offset + 1], mValue[offset + 2],
                        mValue[offset + 3]), 32);
            default:
                return null;
        }
    }

    public Float getFloatValue(int formatType, int offset) {
        if ((offset + getTypeLen(formatType)) > mValue.length) return null;
        switch (formatType) {
            case FORMAT_SFLOAT:
                return bytesToFloat(mValue[offset], mValue[offset + 1]);
            case FORMAT_FLOAT:
                return bytesToFloat(mValue[offset], mValue[offset + 1], mValue[offset + 2], mValue[offset + 3]);
            default:
                return null;
        }
    }

    public String getStringValue(int offset) {
        if (mValue == null || offset > mValue.length) return null;
        byte[] strBytes = new byte[mValue.length - offset];
        for (int i = 0; i != (mValue.length - offset); ++i) strBytes[i] = mValue[offset + i];
        return new String(strBytes);
    }

    public boolean setValue(byte[] value) {
        mValue = value;
        return true;
    }

    @SuppressWarnings("fallthrough")
    public boolean setValue(int value, int formatType, int offset) {
        int len = offset + getTypeLen(formatType);
        if (mValue == null) mValue = new byte[len];
        if (len > mValue.length) return false;
        switch (formatType) {
            case FORMAT_SINT8:
                value = intToSignedBits(value, 8);
                // Fall-through intended, as in AOSP
            case FORMAT_UINT8:
                mValue[offset] = (byte) (value & 0xFF);
                break;
            case FORMAT_SINT16:
                value = intToSignedBits(value, 16);
                // Fall-through intended, as in AOSP
            case FORMAT_UINT16:
                mValue[offset++] = (byte) (value & 0xFF);
                mValue[offset] = (byte) ((value >> 8) & 0xFF);
                break;
            case FORMAT_SINT32:
                value = intToSignedBits(value, 32);
                // Fall-through intended, as in AOSP
            case FORMAT_UINT32:
                mValue[offset++] = (byte) (value & 0xFF);
                mValue[offset++] = (byte) ((value >> 8) & 0xFF);
                mValue[offset++] = (byte) ((value >> 16) & 0xFF);
                mValue[offset] = (byte) ((value >> 24) & 0xFF);
                break;
            default:
                return false;
        }
        return true;
    }

    public boolean setValue(int mantissa, int exponent, int formatType, int offset) {
        int len = offset + getTypeLen(formatType);
        if (mValue == null) mValue = new byte[len];
        if (len > mValue.length) return false;
        switch (formatType) {
            case FORMAT_SFLOAT:
                mantissa = intToSignedBits(mantissa, 12);
                exponent = intToSignedBits(exponent, 4);
                mValue[offset++] = (byte) (mantissa & 0xFF);
                mValue[offset] = (byte) ((mantissa >> 8) & 0x0F);
                mValue[offset] += (byte) ((exponent & 0x0F) << 4);
                break;
            case FORMAT_FLOAT:
                mantissa = intToSignedBits(mantissa, 24);
                exponent = intToSignedBits(exponent, 8);
                mValue[offset++] = (byte) (mantissa & 0xFF);
                mValue[offset++] = (byte) ((mantissa >> 8) & 0xFF);
                mValue[offset++] = (byte) ((mantissa >> 16) & 0xFF);
                mValue[offset] += (byte) (exponent & 0xFF);
                break;
            default:
                return false;
        }
        return true;
    }

    public boolean setValue(String value) {
        mValue = value.getBytes();
        return true;
    }
}
