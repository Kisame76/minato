/*
 * The restrictions service: IRestrictionsManager of system_server, local in
 * the app's process, which ServiceManager's table gives as "restrictions" and AOSP's RestrictionsManager asks. It
 * answers as a phone does for an app that no restrictions provider serves [measured on apk-runner-35: no provider,
 * application restrictions an empty Bundle]. Every other call is AIDL's Default answer. Not under com.android.minato,
 * whose classes tools/framework-host-check.sh compiles on the host without android.*.
 */
package android.content;

import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;

public final class LocalRestrictionsManager extends IRestrictionsManager.Default {
    private final Binder mBinder = new Binder();

    private LocalRestrictionsManager() {
        mBinder.attachInterface(this, IRestrictionsManager.Stub.DESCRIPTOR);
    }

    /** ServiceManager's maker for "restrictions": the Binder that carries a new service. */
    public static IBinder newBinder() {
        return new LocalRestrictionsManager().asBinder();
    }

    @Override
    public IBinder asBinder() {
        return mBinder;
    }

    /** An app that no restrictions provider serves has no restrictions: an empty Bundle, as Android's service gives. */
    @Override
    public Bundle getApplicationRestrictions(String packageName) {
        return new Bundle();
    }
}
