package com.android.internal.compat;

import java.util.Map;

/** Compile-only stub (ADR-0043 test 2), never in framework.jar: CompatChanges makes it for IPlatformCompat. */
public final class CompatibilityOverridesToRemoveByPackageConfig {
    public CompatibilityOverridesToRemoveByPackageConfig(
            Map<String, CompatibilityOverridesToRemoveConfig> packageNameToOverridesToRemove) {
        throw new RuntimeException("stub");
    }
}
