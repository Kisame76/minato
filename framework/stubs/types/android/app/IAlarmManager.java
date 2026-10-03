package android.app;

import android.os.IBinder;
import android.os.RemoteException;

/**
 * Compile-only stub (ADR-0039 b3.5a), never in framework.jar: the alarm service's AIDL interface, which
 * SystemClock.setCurrentTimeMillis asks; an app may not set the time. ADR-0045's local services come with step c.
 */
public interface IAlarmManager {
    boolean setTime(long millis) throws RemoteException;

    abstract class Stub {
        public static IAlarmManager asInterface(IBinder obj) {
            throw new RuntimeException("stub");
        }
    }
}
