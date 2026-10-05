package android.location;

import android.os.IBinder;
import android.os.RemoteException;

/**
 * Compile-only stub (ADR-0039 b3.5a), never in framework.jar: the location service's AIDL interface, which
 * SystemClock.currentGnssTimeClock asks, and Settings.Secure's deprecated isLocationProviderEnabled (ADR-0045, gap
 * 13a); Minato has no location provider (ADR-0045).
 */
public interface ILocationManager {
    LocationTime getGnssTimeMillis() throws RemoteException;

    boolean isProviderEnabledForUser(String provider, int userId) throws RemoteException;

    abstract class Stub {
        public static ILocationManager asInterface(IBinder obj) {
            throw new RuntimeException("stub");
        }
    }
}
