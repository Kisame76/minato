/*
 * The VPN management service (group 1 of the framework breadth, vpn-test): IVpnManager of system_server, local in the
 * app's process, which ServiceManager's table gives as "vpn_management" and VpnService asks. iOS gives Minato no VPN
 * (a packet tunnel is a network extension of its own, outside this runtime), so the service answers as an Android 15
 * phone does for an app the user has not allowed to be a VPN, the one who said no to the consent dialog [verified:
 * vpn-test on apk-runner-35]: prepareVpn is false, so VpnService.prepare hands back the dialog's intent every time,
 * establishVpn gives no tunnel (null) and the address and network calls are refused (false). It is AIDL's Default
 * class, whose other answers are false, null and 0.
 */
package android.net;

import android.os.Binder;
import android.os.IBinder;
import android.os.ParcelFileDescriptor;

public final class LocalVpnService extends IVpnManager.Default {
    private final Binder mBinder = new Binder();

    private LocalVpnService() {
        mBinder.attachInterface(this, IVpnManager.Stub.DESCRIPTOR);
    }

    /** ServiceManager's maker for "vpn_management": the Binder that carries a new service. */
    public static IBinder newBinder() {
        return new LocalVpnService().asBinder();
    }

    @Override
    public IBinder asBinder() {
        return mBinder;
    }

    @Override
    public boolean prepareVpn(String oldPackage, String newPackage, int userId) {
        return false;
    }

    @Override
    public ParcelFileDescriptor establishVpn(com.android.internal.net.VpnConfig config) {
        return null;
    }
}
