/*
 * The activity task manager's interface (ADR-0039 step b3.5b2-1): our class under its AIDL's name, as IWindowManager is
 * (step c2c2), with exactly the methods framework.jar's code calls: Instrumentation's and Activity's starts, the next
 * matching activity, local voice interaction, Activity's screen capture observers, and ActivityClient's controller. AOSP's AIDL has about 130 methods,
 * whose parcelables framework.jar lacks for nothing else. Its Stub's asInterface is AIDL's for the one local service,
 * LocalActivityTaskManager, which ServiceManager's table gives as "activity_task".
 */
package android.app;

import android.content.IIntentSender;
import android.content.Intent;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.IInterface;
import android.os.RemoteException;

public interface IActivityTaskManager extends IInterface {
    int startActivity(IApplicationThread caller, String callingPackage, String callingFeatureId, Intent intent,
            String resolvedType, IBinder resultTo, String resultWho, int requestCode, int flags,
            ProfilerInfo profilerInfo, Bundle options) throws RemoteException;

    boolean startNextMatchingActivity(IBinder callingActivity, Intent intent, Bundle options) throws RemoteException;

    int startActivityIntentSender(IApplicationThread caller, IIntentSender target, IBinder whitelistToken,
            Intent fillInIntent, String resolvedType, IBinder resultTo, String resultWho, int requestCode,
            int flagsMask, int flagsValues, Bundle options) throws RemoteException;

    boolean supportsLocalVoiceInteraction() throws RemoteException;

    void registerScreenCaptureObserver(IBinder activityToken, IScreenCaptureObserver observer) throws RemoteException;

    void unregisterScreenCaptureObserver(IBinder activityToken, IScreenCaptureObserver observer)
            throws RemoteException;

    int startActivities(IApplicationThread caller, String callingPackage, String callingFeatureId, Intent[] intents,
            String[] resolvedTypes, IBinder resultTo, Bundle options, int userId) throws RemoteException;

    int startActivityAsUser(IApplicationThread caller, String callingPackage, String callingFeatureId, Intent intent,
            String resolvedType, IBinder resultTo, String resultWho, int requestCode, int flags,
            ProfilerInfo profilerInfo, Bundle options, int userId) throws RemoteException;

    int startActivityAsCaller(IApplicationThread caller, String callingPackage, Intent intent, String resolvedType,
            IBinder resultTo, String resultWho, int requestCode, int flags, ProfilerInfo profilerInfo, Bundle options,
            boolean ignoreTargetSecurity, int userId) throws RemoteException;

    IActivityClientController getActivityClientController() throws RemoteException;

    /** As AOSP's: the service carried by a Binder under the interface's descriptor. */
    abstract class Stub extends Binder implements IActivityTaskManager {
        public static final String DESCRIPTOR = "android.app.IActivityTaskManager";

        public Stub() {
            attachInterface(this, DESCRIPTOR);
        }

        /** As AOSP's: the local service in {@code binder}, null for none; Minato has no other process to proxy. */
        public static IActivityTaskManager asInterface(IBinder binder) {
            if (binder == null) {
                return null;
            }
            IInterface local = binder.queryLocalInterface(DESCRIPTOR);
            return local instanceof IActivityTaskManager ? (IActivityTaskManager) local : null;
        }

        @Override
        public IBinder asBinder() {
            return this;
        }
    }
}
