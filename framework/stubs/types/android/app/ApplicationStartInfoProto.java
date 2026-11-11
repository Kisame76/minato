package android.app;

import android.util.proto.ProtoStream;

/**
 * Compile-only stub (H5), never in framework.jar: the field ids protoc generates from application_start_info.proto,
 * which ApplicationStartInfo.writeToProto and readFromProto write and read the system's persisted record by. javac
 * inlines them, so the numbers here (the order of the writes) are what that code gets; Minato keeps no such record,
 * so nothing persists or reads one.
 */
public final class ApplicationStartInfoProto {
    public static final long PID = ProtoStream.FIELD_TYPE_INT32 | ProtoStream.FIELD_COUNT_SINGLE | 1L;
    public static final long REAL_UID = ProtoStream.FIELD_TYPE_INT32 | ProtoStream.FIELD_COUNT_SINGLE | 2L;
    public static final long PACKAGE_UID = ProtoStream.FIELD_TYPE_INT32 | ProtoStream.FIELD_COUNT_SINGLE | 3L;
    public static final long DEFINING_UID = ProtoStream.FIELD_TYPE_INT32 | ProtoStream.FIELD_COUNT_SINGLE | 4L;
    public static final long PROCESS_NAME = ProtoStream.FIELD_TYPE_STRING | ProtoStream.FIELD_COUNT_SINGLE | 5L;
    public static final long STARTUP_STATE = ProtoStream.FIELD_TYPE_INT32 | ProtoStream.FIELD_COUNT_SINGLE | 6L;
    public static final long REASON = ProtoStream.FIELD_TYPE_INT32 | ProtoStream.FIELD_COUNT_SINGLE | 7L;
    public static final long STARTUP_TIMESTAMPS = ProtoStream.FIELD_TYPE_BYTES | ProtoStream.FIELD_COUNT_SINGLE | 8L;
    public static final long START_TYPE = ProtoStream.FIELD_TYPE_INT32 | ProtoStream.FIELD_COUNT_SINGLE | 9L;
    public static final long START_INTENT = ProtoStream.FIELD_TYPE_BYTES | ProtoStream.FIELD_COUNT_SINGLE | 10L;
    public static final long LAUNCH_MODE = ProtoStream.FIELD_TYPE_INT32 | ProtoStream.FIELD_COUNT_SINGLE | 11L;
    public static final long WAS_FORCE_STOPPED = ProtoStream.FIELD_TYPE_BOOL | ProtoStream.FIELD_COUNT_SINGLE | 12L;
}
