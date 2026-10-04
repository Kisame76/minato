// Hidden members of the public API's compile-only stub (ADR-0039 step c2c2), which tools/managergen joins with it:
// AnimationUtils.createInterpolatorFromXml makes it with the theme.
import android.content.res.Resources;
import android.content.res.Resources.Theme;
import android.util.AttributeSet;

class OvershootInterpolator {
    /** @hide */
    public OvershootInterpolator(Resources res, Theme theme, AttributeSet attrs) {
        throw new RuntimeException("stub");
    }
}
