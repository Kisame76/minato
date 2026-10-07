/*
 * As AOSP's, which its LoadedApk.java declares beside the class: where a receiver was registered, which LoadedApk's
 * removeContextRegistrations logs when its context goes without unregistering it (ADR-0045 decision 5, step d2).
 */
package android.app;

import android.util.AndroidRuntimeException;

@SuppressWarnings("serial") // As AOSP's, which names no serialVersionUID.
final class IntentReceiverLeaked extends AndroidRuntimeException {
    IntentReceiverLeaked(String msg) {
        super(msg);
    }
}
