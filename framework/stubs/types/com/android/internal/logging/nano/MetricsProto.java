package com.android.internal.logging.nano;

/**
 * Compile-only stub (ADR-0043 step d)), never in framework.jar: the metrics categories, which protoc generates from
 * metrics_constants.proto. javac inlines them, so these values, not the proto's, are what our MetricsLogger and
 * StatusBarNotification's log entry (ADR-0045) and SliceMetrics' (H5) get; none writes anything.
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
        public static final int FIELD_CLASS_NAME = 10;
        public static final int FIELD_SLICE_AUTHORITY = 11;
        public static final int FIELD_SLICE_PATH = 12;
        public static final int FIELD_SUBSLICE_AUTHORITY = 13;
        public static final int FIELD_SUBSLICE_PATH = 14;
        public static final int RESERVED_FOR_LOGBUILDER_BUCKET = 15;
        public static final int RESERVED_FOR_LOGBUILDER_CATEGORY = 16;
        public static final int RESERVED_FOR_LOGBUILDER_LATENCY_MILLIS = 17;
        public static final int RESERVED_FOR_LOGBUILDER_NAME = 18;
        public static final int RESERVED_FOR_LOGBUILDER_PACKAGENAME = 19;
        public static final int RESERVED_FOR_LOGBUILDER_PID = 20;
        public static final int RESERVED_FOR_LOGBUILDER_SUBTYPE = 21;
        public static final int RESERVED_FOR_LOGBUILDER_TIMESTAMP = 22;
        public static final int RESERVED_FOR_LOGBUILDER_TYPE = 23;
        public static final int RESERVED_FOR_LOGBUILDER_UID = 24;
        public static final int RESERVED_FOR_LOGBUILDER_VALUE = 25;
        public static final int SLICE = 26;
        public static final int TYPE_ACTION = 27;
        public static final int TYPE_CLOSE = 28;
        public static final int TYPE_OPEN = 29;
        public static final int TYPE_UNKNOWN = 30;
    }
}
