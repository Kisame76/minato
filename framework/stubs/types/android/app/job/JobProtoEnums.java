package android.app.job;

/**
 * Compile-only stub, never in framework.jar: the proto of
 * frameworks/proto_logging's stats/enums/app/job/job_enums.proto, whose InternalStopReasonEnum values JobParameters
 * names and its switch needs as constants. The numbers are the proto's.
 */
public final class JobProtoEnums {
    public static final int INTERNAL_STOP_REASON_UNKNOWN = -1;
    public static final int INTERNAL_STOP_REASON_CANCELLED = 0;
    public static final int INTERNAL_STOP_REASON_CONSTRAINTS_NOT_SATISFIED = 1;
    public static final int INTERNAL_STOP_REASON_PREEMPT = 2;
    public static final int INTERNAL_STOP_REASON_TIMEOUT = 3;
    public static final int INTERNAL_STOP_REASON_DEVICE_IDLE = 4;
    public static final int INTERNAL_STOP_REASON_DEVICE_THERMAL = 5;
    public static final int INTERNAL_STOP_REASON_RESTRICTED_BUCKET = 6;
    public static final int INTERNAL_STOP_REASON_UNINSTALL = 7;
    public static final int INTERNAL_STOP_REASON_DATA_CLEARED = 8;
    public static final int INTERNAL_STOP_REASON_RTC_UPDATED = 9;
    public static final int INTERNAL_STOP_REASON_SUCCESSFUL_FINISH = 10;
    public static final int INTERNAL_STOP_REASON_USER_UI_STOP = 11;
    public static final int INTERNAL_STOP_REASON_ANR = 12;
    public static final int INTERNAL_STOP_REASON_TIMEOUT_ABANDONED = 13;
}
