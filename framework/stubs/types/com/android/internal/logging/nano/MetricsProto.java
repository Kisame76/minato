package com.android.internal.logging.nano;

/**
 * Compile-only stub (ADR-0043 step d)), never in framework.jar: the metrics categories, which protoc generates from
 * metrics_constants.proto. javac inlines them, so this value, not the proto's, is what our MetricsLogger gets; it
 * writes nothing.
 */
public final class MetricsProto {
    public static final class MetricsEvent {
        public static final int TEXT_LONGPRESS = 1;
    }
}
