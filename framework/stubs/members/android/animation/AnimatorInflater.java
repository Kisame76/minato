// Hidden members of the public API's compile-only stub (ADR-0043 test 2), which tools/managergen joins with it:
// AnimatedVectorDrawable inflates its animators with the path error's scale.

import android.content.res.Resources;

class AnimatorInflater {
    /** @hide */
    public static Animator loadAnimator(Resources resources, Resources.Theme theme, int id, float pathErrorScale)
            throws Resources.NotFoundException {
        throw new RuntimeException("stub");
    }
}
