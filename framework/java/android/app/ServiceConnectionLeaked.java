/*
 * As AOSP's, which its LoadedApk.java declares beside the class: where a service connection was bound, which LoadedApk's
 * removeContextRegistrations logs when its context goes without unbinding it (ADR-0045 step SV3).
 */
package android.app;

import android.util.AndroidRuntimeException;

@SuppressWarnings("serial") // As AOSP's, which names no serialVersionUID.
final class ServiceConnectionLeaked extends AndroidRuntimeException {
    ServiceConnectionLeaked(String msg) {
        super(msg);
    }
}
