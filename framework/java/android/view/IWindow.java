/*
 * Our IWindow (ADR-0039 step c2c3): the window's binder, which View.AttachInfo keeps as the window's token and hands to
 * the window session. AOSP's AIDL has 20 methods, which system_server calls on a window; Minato has no system_server,
 * so ours has none, and ViewRootImpl's W is a plain Binder for the token.
 */
package android.view;

import android.os.IBinder;
import android.os.IInterface;

/** @hide */
public interface IWindow extends IInterface {
    /** As AIDL's Stub: a Binder that is the window. */
    abstract class Stub extends android.os.Binder implements IWindow {
        @Override
        public IBinder asBinder() {
            return this;
        }
    }
}
