// Hidden members of the public API's compile-only stub (ADR-0043 test 2), which tools/managergen joins with it:
// Typeface.Builder asks it for a downloadable font, which needs Google Play's font provider.

import android.graphics.Typeface;

class FontsContract {
    /** @hide */
    public static Typeface getFontSync(FontRequest request) {
        throw new RuntimeException("stub");
    }
}
