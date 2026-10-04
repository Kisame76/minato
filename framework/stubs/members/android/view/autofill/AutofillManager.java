// Hidden members of the public API's compile-only stub (ADR-0039 step c1), which tools/managergen joins with it:
// Context's hidden getAutofillClient and setAutofillClient name the client interface.
// (ADR-0039 step c2c2) View and ViewGroup ask autofill; it is off (ADR-0041).
import android.view.View;

class AutofillManager {
    /** @hide */
    public interface AutofillClient {
    }

    /** @hide */
    public boolean isTriggerFillRequestOnFilteredImportantViewsEnabled() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public boolean isTriggerFillRequestOnUnimportantViewEnabled() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public boolean shouldIncludeAllChildrenViewsWithAutofillTypeNotNoneInAssistStructure() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public boolean shouldIncludeAllChildrenViewInAssistStructure() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public boolean shouldAlwaysIncludeWebviewInAssistStructure() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public boolean shouldIncludeInvisibleViewInAssistStructure() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public boolean isActivityDeniedForAutofill() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public boolean isAutofillable(View view) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public void notifyViewEnteredForFillDialog(View v) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public void notifyViewEnteredForAugmentedAutofill( View view) {
        throw new RuntimeException("stub");
    }
}
