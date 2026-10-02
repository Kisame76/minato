package android.util;

import java.io.PrintWriter;
import java.io.Writer;

/** Compile-only stub (ADR-0039 b3.4), never in framework.jar: ResourcesManager's dump writes through it. */
public class IndentingPrintWriter extends PrintWriter {
    public IndentingPrintWriter(Writer writer, String singleIndent) {
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
