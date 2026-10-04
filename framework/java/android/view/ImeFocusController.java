/*
 * Our ImeFocusController, under AOSP's name (ADR-0039 step c2c3): what a window's ViewRootImpl gives View for its focus
 * changes. AOSP's hands them to InputMethodManager, which shows the soft keyboard for a focused editor; Minato's
 * windows have no input method yet (ADR-0041 decision 2: a local InputMethodManager without a keyboard comes with a)'s
 * input), so a focus change reaches no IME and no view has the IME's focus.
 */
package android.view;

/** @hide */
public final class ImeFocusController {
    ImeFocusController() {
    }

    void onViewFocusChanged(View view, boolean hasFocus) {
    }

    void onViewDetachedFromWindow(View view) {
    }

    boolean hasImeFocus() {
        return false;
    }
}
