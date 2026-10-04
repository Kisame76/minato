package android.window;

/**
 * Compile-only stub (ADR-0039 step c2c2), never in framework.jar: the window's back dispatcher, which ViewGroup asks
 * through an attached view's ViewRootImpl whether a back gesture is in progress. AOSP's comes with the window in step
 * c2c3.
 */
public class WindowOnBackInvokedDispatcher implements OnBackInvokedDispatcher {
    public boolean isBackGestureInProgress() {
        throw new RuntimeException("stub");
    }

    @Override
    public void registerOnBackInvokedCallback(int priority, OnBackInvokedCallback callback) {
        throw new RuntimeException("stub");
    }

    @Override
    public void unregisterOnBackInvokedCallback(OnBackInvokedCallback callback) {
        throw new RuntimeException("stub");
    }
}
