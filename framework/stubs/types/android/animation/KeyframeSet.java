package android.animation;

/**
 * Compile-only stub (ADR-0039 step c2c2), never in framework.jar: LayoutTransition compares a keyframe set's first and
 * last values.
 */
public class KeyframeSet implements Keyframes {
    Keyframe mFirstKeyframe;

    Keyframe mLastKeyframe;

    public Object getValue(float fraction) {
        throw new RuntimeException("stub");
    }
}
