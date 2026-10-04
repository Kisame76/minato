package android.view.contentcapture;

/**
 * Compile-only stub (ADR-0039 step c2c1), never in framework.jar: the data share callback's AIDL interface, which
 * ContentCaptureManager's delegate implements; content capture stays off on Minato: no Context gives a
 * ContentCaptureManager, as on a device without the service.
 */
public interface IDataShareWriteAdapter extends android.os.IInterface {
    void write(android.os.ParcelFileDescriptor destination) throws android.os.RemoteException;

    void error(int errorCode) throws android.os.RemoteException;

    void finish() throws android.os.RemoteException;

    abstract class Stub extends android.os.Binder implements IDataShareWriteAdapter {
        public android.os.IBinder asBinder() {
            throw new RuntimeException("stub");
        }
    }

    void rejected() throws android.os.RemoteException;
}
