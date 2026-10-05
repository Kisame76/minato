/*
 * Ours (ADR-0043 step d), decision 3): com.android.internal.logging.MetricsLogger under AOSP's name, which every
 * Editor makes and TextView and Editor tell of a long press. AOSP's writes to the event log and statsd for the system's
 * metrics, with categories from protos that are not fetched; Minato keeps no such metrics, so ours writes nothing.
 */
package com.android.internal.logging;

import android.content.Context;

/** @hide */
public class MetricsLogger {
    public MetricsLogger() {
    }

    public void action(int category) {
    }

    public void action(int category, int value) {
    }

    public static void action(Context context, int category) {
    }

    public static void action(Context context, int category, int value) {
    }
}
