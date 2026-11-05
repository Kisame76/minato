/*
 * Ours under AOSP's name (services batch D): the client through which an accessibility service's process asks the system
 * for the nodes and windows of an app. AOSP's finds the service's connection by its id and calls over Binder, with a
 * cache of what it was told. Minato has no accessibility service (ADR-0041), so no connection exists: every id is
 * undefined and every query answers as Android's client does for a node without a connection, with null, false or an
 * empty list.
 */
package android.view.accessibility;

import android.os.Bundle;
import android.os.IBinder;

import java.util.Collections;
import java.util.List;

public final class AccessibilityInteractionClient {
    private static final AccessibilityInteractionClient sInstance = new AccessibilityInteractionClient();

    private AccessibilityInteractionClient() {
    }

    public static AccessibilityInteractionClient getInstance() {
        return sInstance;
    }

    public AccessibilityNodeInfo findFocus(int connectionId, int accessibilityWindowId, long accessibilityNodeId,
            int focusType) {
        return null;
    }

    public AccessibilityNodeInfo focusSearch(int connectionId, int accessibilityWindowId, long accessibilityNodeId,
            int direction) {
        return null;
    }

    public AccessibilityNodeInfo findAccessibilityNodeInfoByAccessibilityId(int connectionId,
            int accessibilityWindowId, long accessibilityNodeId, boolean bypassCache, int prefetchFlags,
            Bundle arguments) {
        return null;
    }

    public AccessibilityNodeInfo findAccessibilityNodeInfoByAccessibilityId(int connectionId, IBinder leashToken,
            long accessibilityNodeId, boolean bypassCache, int prefetchFlags, Bundle arguments) {
        return null;
    }

    public boolean performAccessibilityAction(int connectionId, int accessibilityWindowId, long accessibilityNodeId,
            int action, Bundle arguments) {
        return false;
    }

    public List<AccessibilityNodeInfo> findAccessibilityNodeInfosByText(int connectionId, int accessibilityWindowId,
            long accessibilityNodeId, String text) {
        return Collections.emptyList();
    }

    public List<AccessibilityNodeInfo> findAccessibilityNodeInfosByViewId(int connectionId,
            int accessibilityWindowId, long accessibilityNodeId, String viewId) {
        return Collections.emptyList();
    }

    public AccessibilityWindowInfo getWindow(int connectionId, int accessibilityWindowId) {
        return null;
    }

    public AccessibilityWindowInfo getWindow(int connectionId, int accessibilityWindowId, boolean bypassCache) {
        return null;
    }
}
