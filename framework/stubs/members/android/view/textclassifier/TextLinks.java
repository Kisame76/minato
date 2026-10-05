// Hidden members of the public API's compile-only stub (ADR-0043 step d)), which tools/managergen joins with it:
// how LinkMovementMethod opens a link, by touch or by key; javac inlines them, so they carry AOSP's values.
import android.view.View;

class TextLinks {
    public static class TextLinkSpan {
        /** @hide LinkMovementMethod's: a click by touch or by key. */
        public void onClick(View widget, int invocationMethod) {
            throw new RuntimeException("stub");
        }

        /** @hide */
        public static final int INVOCATION_METHOD_TOUCH = 0;

        /** @hide */
        public static final int INVOCATION_METHOD_KEYBOARD = 1;
    }
}
