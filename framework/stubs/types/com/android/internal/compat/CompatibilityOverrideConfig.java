package com.android.internal.compat;

import android.app.compat.PackageOverride;

import java.util.Map;

/** Compile-only stub (ADR-0043 test 2), never in framework.jar: CompatChanges makes it for IPlatformCompat. */
public final class CompatibilityOverrideConfig {
    public CompatibilityOverrideConfig(Map<Long, PackageOverride> overrides) {
        throw new RuntimeException("stub");
    }
}
