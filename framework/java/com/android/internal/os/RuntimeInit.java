/*
 * Our RuntimeInit, under AOSP's name (ADR-0039 step c2d1): the two members framework.jar's code calls. Log.wtf's
 * default handler hands each terrible failure to wtf, after Log.wtf has written it to the app's log at ERROR, and
 * StrictMode's penaltyDropBox names the application object. AOSP's wtf passes the failure to system_server, whose
 * record of it is the system's, not the app's, and the app runs on (app-calls-test, against Android 15). Minato has no
 * system_server, so the app's own line is the whole report.
 */
package com.android.internal.os;

import android.os.IBinder;

public class RuntimeInit {
    /** A terrible failure that Log.wtf has logged: the app runs on, as on Android. */
    public static void wtf(String tag, Throwable t, boolean system) {
    }

    /** The binder that names the app to system_server: none, as Minato has none. */
    public static IBinder getApplicationObject() {
        return null;
    }
}
