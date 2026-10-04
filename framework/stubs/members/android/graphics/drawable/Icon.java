// Hidden members of the public API's compile-only stub (ADR-0039 step c2c3), which tools/managergen joins with it:
// an activity's task description with an in-memory icon, which no activity on Minato can make while Icon is
// compile-only.

import android.graphics.Bitmap;

class Icon {
    /** @hide */
    public Bitmap getBitmap() {
        throw new RuntimeException("stub");
    }
}
