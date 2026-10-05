// Hidden members of the public API's compile-only stub (ADR-0043 step d)), which tools/managergen joins with it:
// Editor's magnifier, which only a drag of the cursor or a selection handle shows.
import android.graphics.drawable.Drawable;
import android.view.View;

class Magnifier {
    /** @hide */
    static Builder createBuilderWithOldMagnifierDefaults(View view) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    void setDrawCursor(boolean enabled, Drawable cursorDrawable) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    void updateSourceFactors(int sourceHeight, float zoom) {
        throw new RuntimeException("stub");
    }

    public static final class Builder {
        /** @hide */
        Builder setFishEyeStyle() {
            throw new RuntimeException("stub");
        }

        /** @hide */
        Builder setSourceSize(int width, int height) {
            throw new RuntimeException("stub");
        }
    }
}
