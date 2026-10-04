// Hidden members of the public API's compile-only stub (ADR-0045, the input service), which tools/managergen joins
// with it: GestureDetector's strategy. javac inlines the default, IInputConstants' at the tag.
class VelocityTracker {
    /** @hide */
    public @interface VelocityTrackerStrategy {
    }

    /** @hide */
    public static final int VELOCITY_TRACKER_STRATEGY_DEFAULT = -1;

    /** @hide */
    public static VelocityTracker obtain(int strategy) {
        throw new RuntimeException("stub");
    }
}
