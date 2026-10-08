package android.os;

/**
 * Compile-only stub (the framework breadth's batch 1), never in framework.jar: WorkSource's dumpDebug names its field
 * constants, which protoc's javastream plugin generates from core/proto. javac inlines them, so these values, distinct
 * but not the proto's field numbers, are what dumpDebug writes; it does not run in an app.
 */
public final class WorkSourceProto {
    public static final long WORK_SOURCE_CONTENTS = 1L;
    public static final long WORK_CHAINS = 2L;

    public static final class WorkSourceContentProto {
        public static final long UID = 1L;
        public static final long NAME = 2L;
    }
}
