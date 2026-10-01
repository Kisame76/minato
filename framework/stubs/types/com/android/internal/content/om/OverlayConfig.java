package com.android.internal.content.om;

/** Compile-only stub (ADR-0039 b3.1), never in framework.jar: AssetManager's zygote path calls it. */
public class OverlayConfig {
    public static OverlayConfig getZygoteInstance() {
        throw new RuntimeException("stub");
    }

    public String[] createImmutableFrameworkIdmapsInZygote() {
        throw new RuntimeException("stub");
    }
}
