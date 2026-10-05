/*
 * AttributionSource's payload (ADR-0045, gap 13a): the class aidl --lang=java generates from frameworks/native's
 * libs/permission/aidl/android/content/AttributionSourceState.aidl at android-15.0.0_r1 [verified: our aidl on that
 * file], which the fetch lacks. AOSP's AttributionSource, which framework.jar takes since ContentProvider's transport
 * names it in every call, stands on it. Its fields and its parcel are the structured parcelable's, in the AIDL's order.
 */
package android.content;

import android.os.BadParcelableException;
import android.os.IBinder;
import android.os.Parcel;
import android.os.Parcelable;

/** @hide */
public class AttributionSourceState implements Parcelable {
    /** The PID that is accessing the permission protected data. */
    public int pid = -1;
    /** The UID that is accessing the permission protected data. */
    public int uid = -1;
    /** The default device ID from where the permission protected data is read. */
    public int deviceId = 0;
    /** The package that is accessing the permission protected data. */
    public String packageName;
    /** The attribution tag of the app accessing the permission protected data. */
    public String attributionTag;
    /** Unique token for that source. */
    public IBinder token;
    /** Permissions that should be considered revoked regardless if granted. */
    public String[] renouncedPermissions;
    /** The next app to receive the permission protected data. */
    public AttributionSourceState[] next;

    public static final Parcelable.Creator<AttributionSourceState> CREATOR =
            new Parcelable.Creator<AttributionSourceState>() {
                @Override
                public AttributionSourceState createFromParcel(Parcel source) {
                    AttributionSourceState state = new AttributionSourceState();
                    state.readFromParcel(source);
                    return state;
                }

                @Override
                public AttributionSourceState[] newArray(int size) {
                    return new AttributionSourceState[size];
                }
            };

    @Override
    public final void writeToParcel(Parcel parcel, int flags) {
        int start = parcel.dataPosition();
        parcel.writeInt(0);
        parcel.writeInt(pid);
        parcel.writeInt(uid);
        parcel.writeInt(deviceId);
        parcel.writeString(packageName);
        parcel.writeString(attributionTag);
        parcel.writeStrongBinder(token);
        parcel.writeStringArray(renouncedPermissions);
        parcel.writeTypedArray(next, flags);
        int end = parcel.dataPosition();
        parcel.setDataPosition(start);
        parcel.writeInt(end - start);
        parcel.setDataPosition(end);
    }

    /** As aidl's: the fields a sender of an older version wrote, each read only while the parcelable's size lasts. */
    public final void readFromParcel(Parcel parcel) {
        int start = parcel.dataPosition();
        int size = parcel.readInt();
        try {
            if (size < 4) {
                throw new BadParcelableException("Parcelable too small");
            }
            if (parcel.dataPosition() - start >= size) {
                return;
            }
            pid = parcel.readInt();
            if (parcel.dataPosition() - start >= size) {
                return;
            }
            uid = parcel.readInt();
            if (parcel.dataPosition() - start >= size) {
                return;
            }
            deviceId = parcel.readInt();
            if (parcel.dataPosition() - start >= size) {
                return;
            }
            packageName = parcel.readString();
            if (parcel.dataPosition() - start >= size) {
                return;
            }
            attributionTag = parcel.readString();
            if (parcel.dataPosition() - start >= size) {
                return;
            }
            token = parcel.readStrongBinder();
            if (parcel.dataPosition() - start >= size) {
                return;
            }
            renouncedPermissions = parcel.createStringArray();
            if (parcel.dataPosition() - start >= size) {
                return;
            }
            next = parcel.createTypedArray(AttributionSourceState.CREATOR);
        } finally {
            if (start > Integer.MAX_VALUE - size) {
                throw new BadParcelableException("Overflow in the size of parcelable");
            }
            parcel.setDataPosition(start + size);
        }
    }

    @Override
    public int describeContents() {
        int mask = 0;
        if (next != null) {
            for (AttributionSourceState state : next) {
                if (state != null) {
                    mask |= state.describeContents();
                }
            }
        }
        return mask;
    }
}
