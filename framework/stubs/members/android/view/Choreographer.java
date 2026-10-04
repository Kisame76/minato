// Hidden members of the public API's compile-only stub (ADR-0043 test 2), which tools/managergen joins with it:
// RenderNodeAnimator, AnimatedVectorDrawable and FallbackLUTInterpolator time their frames with them. javac inlines
// the constant, so it carries AOSP's value.
// (ADR-0039 step c2c2) View's frame time and animation delay.
// (ADR-0039 step c2c2) View's invalidation and drawable scheduling post to the frame.
class Choreographer {
    /** @hide */
    public static final int CALLBACK_ANIMATION = 1;

    /** @hide */
    public void postCallback(int callbackType, Runnable action, Object token) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public long getFrameTime() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public long getFrameIntervalNanos() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public static long subtractFrameDelay(long delayMillis) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public long getLastFrameTimeNanos() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public void postCallbackDelayed(int callbackType, Runnable action, Object token, long delayMillis) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public void removeCallbacks(int callbackType, Runnable action, Object token) {
        throw new RuntimeException("stub");
    }
}
