/*
 * The activity manager's interface (ADR-0045 decision 5, step d2): our class under its AIDL's name, as
 * IActivityTaskManager is (ADR-0039 step b3.5b2-1), with exactly the methods framework.jar's code calls: the receivers
 * and broadcasts of ContextImpl and LoadedApk, BroadcastReceiver's finish, PendingIntent's intent senders, the started
 * and bound services of ContextImpl, Service and ActivityThread (steps SV2 and SV3), and what StrictMode, Activity,
 * Instrumentation, ContentResolver and UserManager ask. AOSP's AIDL has about 300
 * methods, whose parcelables framework.jar lacks for nothing else. Its Stub's asInterface is AIDL's for the one local
 * service, LocalActivityManagerService, which ServiceManager's table gives as "activity".
 */
package android.app;

import android.content.ComponentName;
import android.content.IIntentReceiver;
import android.content.IIntentSender;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.LocusId;
import android.content.pm.ParceledListSlice;
import android.content.pm.ResolveInfo;
import android.net.Uri;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.IInterface;
import android.os.RemoteCallback;
import android.os.RemoteException;
import android.os.StrictMode;

import com.android.internal.os.IResultReceiver;

public interface IActivityManager extends IInterface {
    Intent registerReceiverWithFeature(IApplicationThread caller, String callerPackage, String callingFeatureId,
            String receiverId, IIntentReceiver receiver, IntentFilter filter, String requiredPermission, int userId,
            int flags) throws RemoteException;

    void unregisterReceiver(IIntentReceiver receiver) throws RemoteException;

    int broadcastIntentWithFeature(IApplicationThread caller, String callingFeatureId, Intent intent,
            String resolvedType, IIntentReceiver resultTo, int resultCode, String resultData, Bundle map,
            String[] requiredPermissions, String[] excludePermissions, String[] excludePackages, int appOp,
            Bundle options, boolean serialized, boolean sticky, int userId) throws RemoteException;

    void finishReceiver(IBinder who, int resultCode, String resultData, Bundle map, boolean abortBroadcast,
            int flags) throws RemoteException;

    IIntentSender getIntentSenderWithFeature(int type, String packageName, String featureId, IBinder token,
            String resultWho, int requestCode, Intent[] intents, String[] resolvedTypes, int flags, Bundle options,
            int userId) throws RemoteException;

    void cancelIntentSender(IIntentSender sender) throws RemoteException;

    ActivityManager.PendingIntentInfo getInfoForIntentSender(IIntentSender sender) throws RemoteException;

    boolean registerIntentSenderCancelListenerEx(IIntentSender sender, IResultReceiver receiver)
            throws RemoteException;

    void unregisterIntentSenderCancelListener(IIntentSender sender, IResultReceiver receiver) throws RemoteException;

    int sendIntentSender(IApplicationThread caller, IIntentSender target, IBinder whitelistToken, int code,
            Intent intent, String resolvedType, IIntentReceiver finishedReceiver, String requiredPermission,
            Bundle options) throws RemoteException;

    boolean isIntentSenderTargetedToPackage(IIntentSender sender) throws RemoteException;

    Intent getIntentForIntentSender(IIntentSender sender) throws RemoteException;

    String getTagForIntentSender(IIntentSender sender, String prefix) throws RemoteException;

    ParceledListSlice<ResolveInfo> queryIntentComponentsForIntentSender(IIntentSender sender, int matchFlags)
            throws RemoteException;

    void handleApplicationStrictModeViolation(IBinder app, int penaltyMask, StrictMode.ViolationInfo crashInfo)
            throws RemoteException;

    void registerStrictModeCallback(IBinder binder) throws RemoteException;

    ComponentName startService(IApplicationThread caller, Intent service, String resolvedType,
            boolean requireForeground, String callingPackage, String callingFeatureId, int userId)
            throws RemoteException;

    int stopService(IApplicationThread caller, Intent service, String resolvedType, int userId) throws RemoteException;

    int bindServiceInstance(IApplicationThread caller, IBinder token, Intent service, String resolvedType,
            IServiceConnection connection, long flags, String instanceName, String callingPackage, int userId)
            throws RemoteException;

    boolean unbindService(IServiceConnection connection) throws RemoteException;

    /** ActivityThread's, with the binder of a service's onBind (step SV3). */
    void publishService(IBinder token, Intent intent, IBinder service) throws RemoteException;

    /** ActivityThread's, when a service's onUnbind asks for onRebind (step SV3). */
    void unbindFinished(IBinder token, Intent service, boolean doRebind) throws RemoteException;

    /** ActivityThread's, once a service's callback returned (step SV2). */
    void serviceDoneExecuting(IBinder token, int type, int startId, int res, Intent intent) throws RemoteException;

    boolean stopServiceToken(ComponentName className, IBinder token, int startId) throws RemoteException;

    void setServiceForeground(ComponentName className, IBinder token, int id, Notification notification, int flags,
            int foregroundServiceType) throws RemoteException;

    int getForegroundServiceType(ComponentName className, IBinder token) throws RemoteException;

    boolean shouldServiceTimeOut(ComponentName className, IBinder token) throws RemoteException;

    boolean hasServiceTimeLimitExceeded(ComponentName className, IBinder token) throws RemoteException;

    IBinder peekService(Intent service, String resolvedType, String callingPackage) throws RemoteException;

    void closeSystemDialogs(String reason) throws RemoteException;

    void setActivityLocusContext(ComponentName activity, LocusId locusId, IBinder appToken) throws RemoteException;

    void addInstrumentationResults(IApplicationThread target, Bundle results) throws RemoteException;

    void getMimeTypeFilterAsync(Uri uri, int userId, RemoteCallback resultCallback) throws RemoteException;

    /** UserManager's isUserRunningOrStopping (gap 1, batch 1b). */
    boolean isUserRunning(int userid, int flags) throws RemoteException;

    /** As AOSP's: the service carried by a Binder under the interface's descriptor. */
    abstract class Stub extends Binder implements IActivityManager {
        public static final String DESCRIPTOR = "android.app.IActivityManager";

        public Stub() {
            attachInterface(this, DESCRIPTOR);
        }

        /** As AOSP's: the local service in {@code binder}, null for none; Minato has no other process to proxy. */
        public static IActivityManager asInterface(IBinder binder) {
            if (binder == null) {
                return null;
            }
            IInterface local = binder.queryLocalInterface(DESCRIPTOR);
            return local instanceof IActivityManager ? (IActivityManager) local : null;
        }

        @Override
        public IBinder asBinder() {
            return this;
        }
    }
}
