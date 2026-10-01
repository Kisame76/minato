package android.util;

import android.content.res.Configuration;

/**
 * Compile-only stub (ADR-0039 b3.1), never in framework.jar: CompatibilityInfo.applyOverrideScaleIfNeeded scales it.
 */
public class MergedConfiguration {
    public Configuration getGlobalConfiguration() {
        throw new RuntimeException("stub");
    }

    public Configuration getOverrideConfiguration() {
        throw new RuntimeException("stub");
    }

    public Configuration getMergedConfiguration() {
        throw new RuntimeException("stub");
    }
}
