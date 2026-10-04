package android.view.contentcapture;

/**
 * Compile-only stub (ADR-0039 step c2c1), never in framework.jar: the content capture service's AIDL interface;
 * content capture stays off on Minato: no Context gives a ContentCaptureManager, as on a device without the service.
 */
public interface IContentCaptureManager extends android.os.IInterface {
    void getServiceComponentName(com.android.internal.os.IResultReceiver result) throws android.os.RemoteException;

    void getServiceSettingsActivity(com.android.internal.os.IResultReceiver result) throws android.os.RemoteException;

    void getContentCaptureConditions(String packageName, com.android.internal.os.IResultReceiver result)
            throws android.os.RemoteException;

    void isContentCaptureFeatureEnabled(com.android.internal.os.IResultReceiver result) throws android.os.RemoteException;

    void removeData(DataRemovalRequest request) throws android.os.RemoteException;

    void shareData(DataShareRequest request, IDataShareWriteAdapter adapter) throws android.os.RemoteException;

    void resetTemporaryService(int userId) throws android.os.RemoteException;

    void setTemporaryService(int userId, String serviceName, int duration) throws android.os.RemoteException;

    void setDefaultServiceEnabled(int userId, boolean enabled) throws android.os.RemoteException;

    abstract class Stub extends android.os.Binder implements IContentCaptureManager {
        public static IContentCaptureManager asInterface(android.os.IBinder obj) {
            throw new RuntimeException("stub");
        }
    }
}
