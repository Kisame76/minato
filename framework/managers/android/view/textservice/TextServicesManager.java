// The written part of TextServicesManager (ADR-0043 step d), decision 3), which tools/managergen joins with the API's
// methods: the class TextView names (getSystemService(TextServicesManager.class)) and a spell checker session ends
// itself through. Since services bundle A of the system services gap getSystemService hands the manager out, as
// Android does; there is no spell checker, so isSpellCheckerEnabled says no, which keeps TextView and Editor from
// opening sessions, and newSpellCheckerSession gives none; its hidden members answer as one without a spell checker
// would. [Known difference: the emulator (apk-runner-35-bare) gives a session for a given locale.]
import android.view.inputmethod.InputMethodManager;
import com.android.internal.textservice.ISpellCheckerSessionListener;

class TextServicesManager {
    /** @hide As the tag's, which SystemServiceRegistry calls. */
    public TextServicesManager() {
    }

    /** @hide TextView's and Editor's: no spell checker. */
    public boolean isSpellCheckerEnabled() {
        return false;
    }

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
