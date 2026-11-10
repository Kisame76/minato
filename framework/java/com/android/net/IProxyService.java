package com.android.net;

import android.os.IBinder;
import android.os.IInterface;
import android.os.RemoteException;

/**
 * Ours in place of the connectivity module's com.android.net.IProxyService, an AIDL interface that is not fetched:
 * the proxy service that resolves a proxy auto-config script for android.net.PacProxySelector. Minato has no such
 * service, so Stub.asInterface answers null for the service ServiceManager does not have, which PacProxySelector
 * takes as "no PAC": it then reports no proxy for the host.
 */
public interface IProxyService extends IInterface {
    String resolvePacFile(String host, String url) throws RemoteException;

    void setPacFile(String scriptUrl) throws RemoteException;

    void startPacSystem() throws RemoteException;

    void stopPacSystem() throws RemoteException;

    abstract class Stub implements IProxyService {
        public static IProxyService asInterface(IBinder binder) {
            return binder == null ? null : (IProxyService) binder.queryLocalInterface("com.android.net.IProxyService");
        }
    }
}
