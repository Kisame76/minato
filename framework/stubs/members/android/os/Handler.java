// Hidden members of the public API's compile-only stub (ADR-0043 test 2), which tools/managergen joins with it:
// RenderNodeAnimator makes asynchronous handlers.
class Handler {
    /** @hide */
    public Handler(boolean async) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public Handler(Looper looper, Callback callback, boolean async) {
        throw new RuntimeException("stub");
    }
}
