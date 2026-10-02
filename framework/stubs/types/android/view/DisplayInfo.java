package android.view;

import android.content.res.CompatibilityInfo;
import android.content.res.Configuration;
import android.util.DisplayMetrics;

/**
 * Compile-only stub (ADR-0039 b3.1), never in framework.jar: CompatibilityInfo names it, and ResourcesManager reads a
 * display's metrics from it (b3.4).
 */
public class DisplayInfo {
    public void getAppMetrics(DisplayMetrics outMetrics, DisplayAdjustments displayAdjustments) {
        throw new RuntimeException("stub");
    }

    public void getAppMetrics(DisplayMetrics outMetrics, CompatibilityInfo ci, Configuration configuration) {
        throw new RuntimeException("stub");
    }
}
