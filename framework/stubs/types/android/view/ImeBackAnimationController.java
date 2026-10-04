package android.view;

/**
 * Compile-only stub (ADR-0039 step c2c3), never in framework.jar: WindowOnBackInvokedDispatcher keeps the IME's back
 * animation.
 */
public class ImeBackAnimationController implements android.window.OnBackAnimationCallback {
    public void onBackInvoked() {
        throw new RuntimeException("stub");
    }
}
