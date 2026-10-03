package android.content.pm;

/**
 * Compile-only stub (ADR-0039 b3.5a), never in framework.jar: ApplicationInfo's dumpDebug names its field constants,
 * which protoc's javastream plugin generates from core/proto. javac inlines them, so these values, distinct but not
 * the proto's field numbers, are what dumpDebug writes; it does not run in an app.
 */
public final class ApplicationInfoProto {
    public static final long CLASS_LOADER_NAME = 1L;
    public static final long DATA_DIR = 2L;
    public static final long DETAIL = 3L;
    public static final long FLAGS = 4L;
    public static final long KNOWN_ACTIVITY_EMBEDDING_CERTS = 5L;
    public static final long OVERLAY_PATHS = 6L;
    public static final long PACKAGE = 7L;
    public static final long PERMISSION = 8L;
    public static final long PRIVATE_FLAGS = 9L;
    public static final long PROCESS_NAME = 10L;
    public static final long PUBLIC_SOURCE_DIR = 11L;
    public static final long RESOURCE_DIRS = 12L;
    public static final long SOURCE_DIR = 13L;
    public static final long SPLIT_CLASS_LOADER_NAMES = 14L;
    public static final long SPLIT_PUBLIC_SOURCE_DIRS = 15L;
    public static final long SPLIT_SOURCE_DIRS = 16L;
    public static final long THEME = 17L;
    public static final long UID = 18L;
    public static final long VERSION = 19L;

    public static final class Version {
        public static final long ENABLED = 20L;
        public static final long MIN_SDK_VERSION = 21L;
        public static final long TARGET_SANDBOX_VERSION = 22L;
        public static final long TARGET_SDK_VERSION = 23L;
        public static final long VERSION_CODE = 24L;
    }

    public static final class Detail {
        public static final long ALLOW_CROSS_UID_ACTIVITY_SWITCH_FROM_BELOW = 25L;
        public static final long CATEGORY = 26L;
        public static final long CLASS_NAME = 27L;
        public static final long COMPATIBLE_WIDTH_LIMIT_DP = 28L;
        public static final long CONTENT = 29L;
        public static final long CREDENTIAL_PROTECTED_DATA_DIR = 30L;
        public static final long DESCRIPTION_RES = 31L;
        public static final long DEVICE_PROTECTED_DATA_DIR = 32L;
        public static final long ENABLE_GWP_ASAN = 33L;
        public static final long ENABLE_MEMTAG = 34L;
        public static final long IS_FULL_BACKUP = 35L;
        public static final long LARGEST_WIDTH_LIMIT_DP = 36L;
        public static final long MANAGE_SPACE_ACTIVITY_NAME = 37L;
        public static final long NATIVE_HEAP_ZERO_INIT = 38L;
        public static final long NETWORK_SECURITY_CONFIG_RES = 39L;
        public static final long REQUIRES_SMALLEST_WIDTH_DP = 40L;
        public static final long SEINFO = 41L;
        public static final long SEINFO_USER = 42L;
        public static final long SHARED_LIBRARY_FILES = 43L;
        public static final long SUPPORTS_RTL = 44L;
        public static final long TASK_AFFINITY = 45L;
        public static final long UI_OPTIONS = 46L;
    }
}
