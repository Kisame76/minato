// Hidden members of the public API's compile-only stub (ADR-0039 b3.1), which tools/managergen joins with it:
// ActivityInfo's dump calls it.
import android.util.Printer;

class ComponentInfo {
    /** @hide */
    protected void dumpBack(Printer pw, String prefix, int dumpFlags) {
        throw new RuntimeException("stub");
    }
}
