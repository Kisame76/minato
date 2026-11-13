package android.os;

/**
 * Compile-only stub (ADR-0045), never in framework.jar: the proto of core/proto/android/os/
 * powermanager.proto, whose WakeLock field IDs PowerManager.WakeLock.dumpDebug writes; javac inlines them. Each ID is
 * ProtoStream's: count single (1 << 40), the field's type << 32, its number.
 */
public final class PowerManagerProto {
    public static final class WakeLock {
        public static final long TAG = 0x10900000001L;
        public static final long PACKAGE_NAME = 0x10900000002L;
        public static final long HELD = 0x10800000003L;
        public static final long INTERNAL_COUNT = 0x10500000004L;
        public static final long WORK_SOURCE = 0x10b00000005L;
    }
}
