package com.android.internal.util;

import java.io.PrintWriter;
import java.io.Writer;

/** Compile-only stub (ADR-0039 b3.5a), never in framework.jar: BaseBundle's dumpStats writes through it. */
public class IndentingPrintWriter extends PrintWriter {
    public IndentingPrintWriter(Writer writer) {
        super(writer);
        throw new RuntimeException("stub");
    }

    public IndentingPrintWriter increaseIndent() {
        throw new RuntimeException("stub");
    }

    public IndentingPrintWriter decreaseIndent() {
        throw new RuntimeException("stub");
    }

    public IndentingPrintWriter print(String key, Object value) {
        throw new RuntimeException("stub");
    }
}
