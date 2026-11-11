package com.android.internal.net;

import android.os.Parcel;
import android.os.Parcelable;

/**
 * Compile-only stub (group 1 of the framework breadth), never in framework.jar: the taken IVpnManager names it in two
 * methods that the local vpn service never answers; AOSP's class drags in the IKE and VCN packages. A Parcelable with
 * a CREATOR, which the generated interface's code asks for.
 */
public class VpnProfile implements Parcelable {
    public static final Creator<VpnProfile> CREATOR = null;

    @Override
    public int describeContents() {
        return 0;
    }

    @Override
    public void writeToParcel(Parcel out, int flags) {
    }
}
