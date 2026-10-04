package android.window;

/**
 * Compile-only stub (ADR-0039 step c2c3), never in framework.jar: WindowOnBackInvokedDispatcher hands its top callback
 * to the window session with it.
 */
public final class OnBackInvokedCallbackInfo {
    public OnBackInvokedCallbackInfo(IOnBackInvokedCallback callback, int priority, boolean isAnimationCallback) {
        throw new RuntimeException("stub");
    }
}
