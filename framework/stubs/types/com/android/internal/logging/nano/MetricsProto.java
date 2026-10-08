package com.android.internal.logging.nano;

/**
 * Compile-only stub (ADR-0043 step d)), never in framework.jar: the metrics categories, which protoc generates from
 * metrics_constants.proto. javac inlines them, so these values, not the proto's, are what our MetricsLogger and
 * StatusBarNotification's log entry (ADR-0045 gap 1, batch 2), which only the system's logging makes, get; neither
 * writes anything.
 */
public final class MetricsProto {
    public static final class MetricsEvent {
        public static final int TEXT_LONGPRESS = 1;
        public static final int VIEW_UNKNOWN = 0;
        public static final int NOTIFICATION_ID = 2;
        public static final int NOTIFICATION_TAG = 3;
        public static final int FIELD_NOTIFICATION_CATEGORY = 4;
        public static final int FIELD_NOTIFICATION_CHANNEL_ID = 5;
        public static final int FIELD_NOTIFICATION_GROUP_ID = 6;
        public static final int FIELD_NOTIFICATION_GROUP_SUMMARY = 7;
        public static final int FIELD_NOTIFICATION_STYLE = 8;
        public static final int FIELD_NOTIFICATION_PEOPLE = 9;
    }
}
