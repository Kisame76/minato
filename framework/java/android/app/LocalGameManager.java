/*
 * The game service (gap 1 of the compatibility plan, batch A): IGameManagerService of system_server, local in the app's
 * process, which ServiceManager's table gives as "game" and AOSP's GameManager asks. It answers as a phone does for an
 * app that is no game and has no game mode configuration [read: GameManagerService, android-15.0.0_r1; measured by
 * system-services-test on apk-runner-35]: the game mode is GAME_MODE_UNSUPPORTED, there is no game mode info, ANGLE is
 * not enabled, and a listener is kept and never called. Every other call is AIDL's Default answer. Not under
 * com.android.minato, whose classes tools/framework-host-check.sh compiles on the host without android.*.
 */
package android.app;

import android.os.Binder;
import android.os.IBinder;

public final class LocalGameManager extends IGameManagerService.Default {
    private final Binder mBinder = new Binder();

    private LocalGameManager() {
        mBinder.attachInterface(this, IGameManagerService.Stub.DESCRIPTOR);
    }

    /** ServiceManager's maker for "game": the Binder that carries a new service. */
    public static IBinder newBinder() {
        return new LocalGameManager().asBinder();
    }

    @Override
    public IBinder asBinder() {
        return mBinder;
    }

    @Override
    public int getGameMode(String packageName, int userId) {
        return GameManager.GAME_MODE_UNSUPPORTED;
    }
}
