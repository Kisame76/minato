package android.util;

import java.io.PrintWriter;
import java.io.Writer;

/**
 * Compile-only stub (ADR-0039 b3.4), never in framework.jar: ResourcesManager's dump writes through it, and since step
 * c2c3 Activity's dumpables (DumpableContainerImpl).
 */
public class IndentingPrintWriter extends PrintWriter {
    public IndentingPrintWriter(Writer writer, String singleIndent) {
        super(writer);
        throw new RuntimeException("stub");
    }

    public IndentingPrintWriter(Writer writer, String singleIndent, String prefix) {
        super(writer);
        throw new RuntimeException("stub");
    }

    public IndentingPrintWriter increaseIndent() {
        throw new RuntimeException("stub");
    }

    public IndentingPrintWriter decreaseIndent() {
        throw new RuntimeException("stub");
    }
}
