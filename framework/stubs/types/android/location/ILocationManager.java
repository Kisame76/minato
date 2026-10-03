package android.location;

import android.os.IBinder;
import android.os.RemoteException;

/**
 * Compile-only stub (ADR-0039 b3.5a), never in framework.jar: the location service's AIDL interface, which
 * SystemClock.currentGnssTimeClock asks; Minato has no location provider (ADR-0045).
 */
public interface ILocationManager {
    LocationTime getGnssTimeMillis() throws RemoteException;

    abstract class Stub {
        public static ILocationManager asInterface(IBinder obj) {
            throw new RuntimeException("stub");
        }
    }
}
