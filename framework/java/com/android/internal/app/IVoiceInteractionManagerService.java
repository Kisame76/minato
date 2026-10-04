/*
 * Our IVoiceInteractionManagerService (ADR-0039 step c2c3): the one method and asInterface that Activity's
 * onPostCreate and onPostResume call. They ask ServiceManager for "voiceinteraction", which Minato's table has none
 * of, so asInterface gets null and Activity returns, as on a device without voice interaction
 * (Activity.java:1771-1787). AOSP's AIDL has 54 methods and 25 imports for that one call.
 */
package com.android.internal.app;

import android.os.IBinder;
import android.os.IInterface;
import android.os.RemoteException;

/** @hide */
public interface IVoiceInteractionManagerService extends IInterface {
    String DESCRIPTOR = "com.android.internal.app.IVoiceInteractionManagerService";

    void notifyActivityEventChanged(IBinder activityToken, int type) throws RemoteException;

    /** As AIDL's: the local service behind a Binder, or null. */
    abstract class Stub extends android.os.Binder implements IVoiceInteractionManagerService {
        public static IVoiceInteractionManagerService asInterface(IBinder obj) {
            if (obj == null) {
                return null;
            }
            IInterface local = obj.queryLocalInterface(DESCRIPTOR);
            return local instanceof IVoiceInteractionManagerService ? (IVoiceInteractionManagerService) local : null;
        }
    }
}
