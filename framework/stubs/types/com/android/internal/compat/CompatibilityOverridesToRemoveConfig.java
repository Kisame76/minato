package com.android.internal.compat;

import java.util.Set;

/** Compile-only stub (ADR-0043 test 2), never in framework.jar: CompatChanges makes it for IPlatformCompat. */
public final class CompatibilityOverridesToRemoveConfig {
    public CompatibilityOverridesToRemoveConfig(Set<Long> changeIds) {
        throw new RuntimeException("stub");
    }
}
