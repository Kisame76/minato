// The written part of TextServicesManager (ADR-0043 step d), decision 3), which tools/managergen joins with the API's
// methods: the class TextView names (getSystemService(TextServicesManager.class)) and a spell checker session ends
// itself through. Text services are absent, so getSystemService answers null and no manager is made; its hidden
// members answer as one without a spell checker would.
import android.view.inputmethod.InputMethodManager;
import com.android.internal.textservice.ISpellCheckerSessionListener;

class TextServicesManager {
    /** @hide TextView's: no spell checker, so no subtype. */
    public SpellCheckerSubtype getCurrentSpellCheckerSubtype(boolean allowImplicitlySelectedSubtype) {
        return null;
    }

    /** @hide SpellCheckerSession's: no service to finish. */
    void finishSpellCheckerService(ISpellCheckerSessionListener listener) {
    }

    /** @hide SpellCheckerSession's. */
    public InputMethodManager getInputMethodManager() {
        return InputMethodManager.peekInstance();
    }
}
