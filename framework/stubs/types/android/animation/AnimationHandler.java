package android.animation;

/**
 * Compile-only stub (ADR-0043 test 2), never in framework.jar: Animator's setAnimatorPausingEnabled forwards to it,
 * which only a test calls; RenderNodeAnimator stands on Animator.
 */
public class AnimationHandler {
    public static void setAnimatorPausingEnabled(boolean enable) {
        throw new RuntimeException("stub");
    }

    public static void setOverrideAnimatorPausingSystemProperty(boolean enable) {
        throw new RuntimeException("stub");
    }
}
