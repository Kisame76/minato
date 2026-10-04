package android.window;

/**
 * Compile-only stub (ADR-0039 step c2c2), never in framework.jar: WindowOnBackInvokedDispatcher hands the IME's
 * callbacks to it.
 */
public class ImeOnBackInvokedDispatcher {
    public void registerOnBackInvokedCallback(int priority, OnBackInvokedCallback callback) {
        throw new RuntimeException("stub");
    }

    public void unregisterOnBackInvokedCallback(OnBackInvokedCallback callback) {
        throw new RuntimeException("stub");
    }
}
