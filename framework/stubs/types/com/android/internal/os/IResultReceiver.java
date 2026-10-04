package com.android.internal.os;

/**
 * Compile-only stub (ADR-0039 step c2c1), never in framework.jar: the system services' one-way answer, which
 * SyncResultReceiver implements; content capture stays off on Minato: no Context gives a ContentCaptureManager, as on
 * a device without the service.
 */
public interface IResultReceiver extends android.os.IInterface {
    void send(int resultCode, android.os.Bundle resultData) throws android.os.RemoteException;

    abstract class Stub extends android.os.Binder implements IResultReceiver {
        public android.os.IBinder asBinder() {
            throw new RuntimeException("stub");
        }
    }
}
