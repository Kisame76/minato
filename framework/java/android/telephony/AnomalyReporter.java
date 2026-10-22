/*
 * The anomaly reporter of the telephony code (the cell identities report an invalid operator code through it).
 * AOSP's class (frameworks/base, telephony) is initialised by the telephony process, which holds MODIFY_PHONE_STATE,
 * and writes statsd atoms and sends ACTION_ANOMALY_REPORTED to a debug package; a process that never initialised it,
 * as every app process, logs the event and drops it. Minato has no telephony process, no statsd and no debug package,
 * so every process is such a process: reportAnomaly takes AOSP's first two steps (the log lines) and stops where AOSP
 * does for an uninitialised reporter, and initialize keeps AOSP's argument and permission checks.
 */
package android.telephony;

import static android.telephony.TelephonyManager.UNKNOWN_CARRIER_ID;

import android.annotation.NonNull;
import android.annotation.RequiresPermission;
import android.content.Context;

import com.android.telephony.Rlog;

import java.util.UUID;

/** @hide */
public final class AnomalyReporter {
    private static final String TAG = "AnomalyReporter";

    private AnomalyReporter() {
    }

    /** Reports an anomaly with an unknown carrier. */
    public static void reportAnomaly(@NonNull UUID eventId, String description) {
        reportAnomaly(eventId, description, UNKNOWN_CARRIER_ID);
    }

    /** Logs the event and drops it: this process never initialised the reporter. */
    public static void reportAnomaly(@NonNull UUID eventId, String description, int carrierId) {
        Rlog.i(TAG, "reportAnomaly: Received anomaly event report with eventId= " + eventId
                + " and description= " + description);
        Rlog.w(TAG, "AnomalyReporter not yet initialized, dropping event=" + eventId);
    }

    /** AOSP's checks; the reporter has nothing to set up here. */
    @RequiresPermission(android.Manifest.permission.MODIFY_PHONE_STATE)
    public static void initialize(@NonNull Context context) {
        if (context == null) {
            throw new IllegalArgumentException("AnomalyReporter needs a non-null context.");
        }
        context.enforceCallingOrSelfPermission(android.Manifest.permission.MODIFY_PHONE_STATE,
                "This app does not have privileges to send debug events");
    }
}
