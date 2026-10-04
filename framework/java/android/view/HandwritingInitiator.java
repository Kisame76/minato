/*
 * Our HandwritingInitiator, under AOSP's name (ADR-0039 step c2c3): what a window's ViewRootImpl gives View for stylus
 * handwriting into an editor. AOSP's starts the input method's handwriting mode; an iPhone has no stylus and Minato's
 * windows no input method yet (ADR-0041 a)'s input), so the editors' focus and handwriting areas go nowhere.
 */
package android.view;

/** @hide */
public class HandwritingInitiator {
    HandwritingInitiator() {
    }

    public void onDelegateViewFocused(View view) {
    }

    public void onEditorFocused(View view) {
    }

    public void updateHandwritingAreasForView(View view) {
    }
}
