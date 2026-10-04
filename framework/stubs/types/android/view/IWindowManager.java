package android.view;

import android.os.IBinder;
import android.os.RemoteException;

/**
 * Compile-only stub (ADR-0039 step c1), never in framework.jar: the window manager's AIDL interface, which
 * StrictMode asks to flash the screen for penaltyFlash.
 */
public interface IWindowManager {
    void showStrictModeViolation(boolean on) throws RemoteException;

    abstract class Stub {
        public static IWindowManager asInterface(IBinder obj) {
            throw new RuntimeException("stub");
        }
    }
}
