// Hidden members of the public API's compile-only stub (ADR-0039 step c1), which tools/managergen joins
// with it: StrictMode's ViolationInfo counts the running animations.
class ValueAnimator {
    /** @hide */
    public static int getCurrentAnimationsCount() {
        throw new RuntimeException("stub");
    }
}
