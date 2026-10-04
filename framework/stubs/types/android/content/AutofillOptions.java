package android.content;

/**
 * Compile-only stub (ADR-0039 step c1), never in framework.jar: Context's hidden getAutofillOptions and
 * setAutofillOptions name it, and isAutofillCompatibilityEnabled reads its compat mode; autofill is off in Minato
 * (ADR-0041). View asks it whether augmented autofill is on (step c2c2).
 */
public final class AutofillOptions {
    public boolean compatModeEnabled;

    public boolean isAugmentedAutofillEnabled(Context context) {
        throw new RuntimeException("stub");
    }
}
