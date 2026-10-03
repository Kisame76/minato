package android.app.timedetector;

import android.app.time.UnixEpochTime;
import android.os.IBinder;
import android.os.RemoteException;

/**
 * Compile-only stub (ADR-0039 b3.5a), never in framework.jar: the time detector's AIDL interface, which
 * SystemClock.currentNetworkTimeMillis asks; Minato has no network time (ADR-0045).
 */
public interface ITimeDetectorService {
    UnixEpochTime latestNetworkTime() throws RemoteException;

    abstract class Stub {
        public static ITimeDetectorService asInterface(IBinder obj) {
            throw new RuntimeException("stub");
        }
    }
}
