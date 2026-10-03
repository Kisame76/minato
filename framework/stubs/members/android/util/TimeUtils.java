// Hidden members of the public API's compile-only stub (ADR-0043 test 2), which tools/managergen joins with it:
// AnimatedVectorDrawable and FallbackLUTInterpolator convert frame times. javac inlines the constant, so it carries
// AOSP's value.
class TimeUtils {
    /** @hide */
    public static final long NANOS_PER_MS = 1000000;
}
