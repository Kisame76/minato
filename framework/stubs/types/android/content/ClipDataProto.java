package android.content;

/**
 * Compile-only stub (the framework breadth's batch 1), never in framework.jar: ClipData's dumpDebug names its field
 * constants, which protoc's javastream plugin generates from core/proto. javac inlines them, so these values, distinct
 * but not the proto's field numbers, are what dumpDebug writes; it does not run in an app.
 */
public final class ClipDataProto {
    public static final long DESCRIPTION = 1L;
    public static final long ICON = 2L;
    public static final long ITEMS = 3L;

    public static final class Icon {
        public static final long WIDTH = 1L;
        public static final long HEIGHT = 2L;
    }

    public static final class Item {
        public static final long HTML_TEXT = 1L;
        public static final long TEXT = 2L;
        public static final long URI = 3L;
        public static final long INTENT = 4L;
        public static final long NOTHING = 5L;
    }
}
