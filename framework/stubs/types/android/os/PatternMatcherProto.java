package android.os;

/**
 * Compile-only stub (ADR-0039 b3.5a), never in framework.jar: PatternMatcher's dumpDebug names its field constants,
 * which protoc's javastream plugin generates from core/proto. javac inlines them, so these values, distinct but not
 * the proto's field numbers, are what dumpDebug writes; it does not run in an app.
 */
public final class PatternMatcherProto {
    public static final long PATTERN = 1L;
    public static final long TYPE = 2L;
}
