/*
 * Our window manager interface, under its AIDL's name (ADR-0039 step c2c2): exactly the methods framework.jar's code
 * calls, which WindowManagerGlobal's local stub answers (ADR-0045 decision 3), as Minato has no system_server window
 * manager. The AIDL is not taken, as its parcelables' imports are not fetched; later steps add methods as their code
 * calls them.
 */
package android.view;

import android.os.IBinder;
import android.os.RemoteException;

public interface IWindowManager {
    void showStrictModeViolation(boolean on) throws RemoteException;

    boolean hasNavigationBar(int displayId) throws RemoteException;

    int getPreferredOptionsPanelGravity(int displayId) throws RemoteException;

    void setInTouchModeOnAllDisplays(boolean inTouch) throws RemoteException;

    void syncInputTransactions(boolean waitForAnimations) throws RemoteException;

    /** What the service manager's "window" binder becomes: the local stub. */
    abstract class Stub {
        public static IWindowManager asInterface(IBinder obj) {
            return WindowManagerGlobal.getWindowManagerService();
        }
    }
}
