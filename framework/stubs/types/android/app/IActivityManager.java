package android.app;

import android.os.IBinder;
import android.os.RemoteException;
import android.os.StrictMode;

/**
 * Compile-only stub (ADR-0039 step c1), never in framework.jar: the activity manager's AIDL interface,
 * which StrictMode asks to dropbox a violation and to register its unsafe intent callback, and which Service and
 * BroadcastReceiver report to (step c2c1); ADR-0045's local services answer it from step c.
 */
public interface IActivityManager {
    void handleApplicationStrictModeViolation(IBinder app, int penaltyMask, StrictMode.ViolationInfo crashInfo)
            throws RemoteException;

    void registerStrictModeCallback(IBinder binder) throws RemoteException;

    boolean stopServiceToken(android.content.ComponentName className, android.os.IBinder token, int startId)
            throws android.os.RemoteException;

    void setServiceForeground(android.content.ComponentName className, android.os.IBinder token, int id,
            android.app.Notification notification, int flags, int foregroundServiceType)
            throws android.os.RemoteException;

    int getForegroundServiceType(android.content.ComponentName className, android.os.IBinder token)
            throws android.os.RemoteException;

    boolean shouldServiceTimeOut(android.content.ComponentName className, android.os.IBinder token)
            throws android.os.RemoteException;

    boolean hasServiceTimeLimitExceeded(android.content.ComponentName className, android.os.IBinder token)
            throws android.os.RemoteException;

    void finishReceiver(android.os.IBinder who, int resultCode, String resultData, android.os.Bundle map,
            boolean abortBroadcast, int flags) throws android.os.RemoteException;

    android.os.IBinder peekService(android.content.Intent service, String resolvedType, String callingPackage)
            throws android.os.RemoteException;
}
