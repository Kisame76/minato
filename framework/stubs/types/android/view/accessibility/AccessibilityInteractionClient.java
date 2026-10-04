package android.view.accessibility;

import android.os.Bundle;

/**
 * Compile-only stub (ADR-0039 step c2c2), never in framework.jar: the client an accessibility service's process asks
 * for nodes, which AccessibilityRecord.getSource calls; accessibility is off on Minato (ADR-0041), so no service asks.
 */
public final class AccessibilityInteractionClient {
    public static AccessibilityInteractionClient getInstance() {
        throw new RuntimeException("stub");
    }

    public AccessibilityNodeInfo findAccessibilityNodeInfoByAccessibilityId(int connectionId,
            int accessibilityWindowId, long accessibilityNodeId, boolean bypassCache, int prefetchFlags,
            Bundle arguments) {
        throw new RuntimeException("stub");
    }
}
