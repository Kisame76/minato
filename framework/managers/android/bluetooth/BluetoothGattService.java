// The written part of BluetoothGattService (services batch E): a GATT service an app builds for its own server or reads
// from a remote one: a UUID, a type, its characteristics and its included services, as AOSP's data holder keeps them
// [verified: bluetooth-gatt-test].
import java.util.ArrayList;
import java.util.List;
import java.util.UUID;

class BluetoothGattService {
    private final UUID mUuid;
    private final int mServiceType;
    private int mInstanceId;
    private final List<BluetoothGattCharacteristic> mCharacteristics = new ArrayList<>();
    private final List<BluetoothGattService> mIncludedServices = new ArrayList<>();

    public BluetoothGattService(UUID uuid, int serviceType) {
        mUuid = uuid;
        mServiceType = serviceType;
    }

    public boolean addService(BluetoothGattService service) {
        mIncludedServices.add(service);
        return true;
    }

    public boolean addCharacteristic(BluetoothGattCharacteristic characteristic) {
        mCharacteristics.add(characteristic);
        characteristic.setService(this);
        return true;
    }

    public UUID getUuid() {
        return mUuid;
    }

    public int getInstanceId() {
        return mInstanceId;
    }

    /** @hide */
    public void setInstanceId(int instanceId) {
        mInstanceId = instanceId;
    }

    public int getType() {
        return mServiceType;
    }

    public List<BluetoothGattService> getIncludedServices() {
        return mIncludedServices;
    }

    public List<BluetoothGattCharacteristic> getCharacteristics() {
        return mCharacteristics;
    }

    public BluetoothGattCharacteristic getCharacteristic(UUID uuid) {
        for (BluetoothGattCharacteristic characteristic : mCharacteristics) {
            if (uuid.equals(characteristic.getUuid())) {
                return characteristic;
            }
        }
        return null;
    }

    public int describeContents() {
        return 0;
    }

    public void writeToParcel(android.os.Parcel out, int flags) {
        out.writeParcelable(new android.os.ParcelUuid(mUuid), 0);
        out.writeInt(mInstanceId);
        out.writeInt(mServiceType);
    }
}
