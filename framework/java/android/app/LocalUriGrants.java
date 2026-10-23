/*
 * The URI grants service (ADR-0057 step b): IUriGrantsManager of system_server, local in the app's process, which
 * ServiceManager's table gives as "uri_grants" and UriGrantsManager, so ContentResolver's
 * takePersistableUriPermission, releasePersistableUriPermission and getPersistedUriPermissions, ask. It keeps the
 * grants of the app only, as UriGrantsManagerService keeps them for a package [verified: documents-test on
 * apk-runner-35]: a URI without a grant makes takePersistableUriPermission throw a SecurityException, and
 * getPersistedUriPermissions lists only the grants the app kept. The grants are com.android.internal.minato.picker's
 * UriGrants, which the system picker fills; the rest of the service is AIDL's Default answer. Not under
 * com.android.minato, whose classes tools/framework-host-check.sh compiles without android.*.
 */
package android.app;

import android.content.UriPermission;
import android.content.pm.ParceledListSlice;
import android.net.Uri;
import android.os.Binder;
import android.os.IBinder;

import com.android.internal.minato.picker.UriGrants;

import java.util.ArrayList;
import java.util.List;

public final class LocalUriGrants extends IUriGrantsManager.Default {
    private final Binder mBinder = new Binder();

    private LocalUriGrants() {
        mBinder.attachInterface(this, IUriGrantsManager.DESCRIPTOR);
    }

    /** ServiceManager's maker for "uri_grants": the Binder that carries a new service. */
    public static IBinder newBinder() {
        return new LocalUriGrants().asBinder();
    }

    @Override
    public IBinder asBinder() {
        return mBinder;
    }

    @Override
    public void takePersistableUriPermission(Uri uri, int modeFlags, String toPackage, int userId) {
        UriGrants.take(uri, modeFlags);
    }

    @Override
    public void releasePersistableUriPermission(Uri uri, int modeFlags, String toPackage, int userId) {
        UriGrants.release(uri, modeFlags);
    }

    /** The grants kept for the app; the app grants nothing to others, so the outgoing ones are none. */
    @Override
    @SuppressWarnings("rawtypes")
    public ParceledListSlice getUriPermissions(String packageName, boolean incoming, boolean persistedOnly) {
        final List<UriPermission> list = incoming ? UriGrants.persisted() : new ArrayList<>();
        return new ParceledListSlice<>(list);
    }

    @Override
    @SuppressWarnings("rawtypes")
    public ParceledListSlice getGrantedUriPermissions(String packageName, int userId) {
        return new ParceledListSlice<>(new ArrayList<GrantedUriPermission>());
    }
}
