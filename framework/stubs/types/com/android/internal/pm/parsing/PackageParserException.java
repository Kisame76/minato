package com.android.internal.pm.parsing;

/** Compile-only stub (ADR-0039 b3.5a), never in framework.jar: PackageParser2's stub throws it. */
public class PackageParserException extends Exception {
    private static final long serialVersionUID = 1;

    public PackageParserException(int error, String detailMessage) {
        super(detailMessage);
        throw new RuntimeException("stub");
    }
}
