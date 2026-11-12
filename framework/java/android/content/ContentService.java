/*
 * The content service (ADR-0045 decisions 2 and 4, gap 13a): the IContentService of Android's ContentService, local
 * in the app's process, which ServiceManager's table gives as "content", for ContentResolver's observers,
 * notifications and syncs. It is AIDL's Default class, as the input service is: an observer is accepted and never
 * called back, and notifyChange reaches none, a listed gap: no setting changes yet, but the app's own providers'
 * notifications reach no observer in the app either, such as a cursor's (Cursor.setNotificationUri, CursorLoader's
 * requery), which AOSP's ObserverNode would serve in the process. The syncs answer AIDL's defaults, as
 * for an app without accounts. Not under com.android.minato, whose classes tools/framework-host-check.sh compiles
 * without android.*.
 */
package android.content;

import android.os.Binder;
import android.os.IBinder;

/** @hide */
public final class ContentService extends IContentService.Default {
    private final Binder mBinder = new Binder();

    private ContentService() {
        mBinder.attachInterface(this, IContentService.Stub.DESCRIPTOR);
    }

    /** ServiceManager's maker for "content": the Binder that carries a new service. */
    public static IBinder newBinder() {
        return new ContentService().asBinder();
    }

    @Override
    public IBinder asBinder() {
        return mBinder;
    }
}
