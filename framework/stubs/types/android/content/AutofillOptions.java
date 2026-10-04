package android.content;

/**
 * Compile-only stub (ADR-0039 step c1), never in framework.jar: Context's hidden getAutofillOptions and
 * setAutofillOptions name it, and isAutofillCompatibilityEnabled reads its compat mode; autofill is off in Minato
 * (ADR-0041).
 */
public final class AutofillOptions {
    public boolean compatModeEnabled;
}
