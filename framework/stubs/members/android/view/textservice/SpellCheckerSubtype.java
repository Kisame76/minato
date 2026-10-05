// Hidden members of the public API's compile-only stub (ADR-0043 step d)), which tools/managergen joins with it: TextView's
// spell checker locale.
import java.util.Locale;

class SpellCheckerSubtype {
    /** @hide */
    public Locale getLocaleObject() {
        throw new RuntimeException("stub");
    }
}
