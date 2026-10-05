// Hidden members of the public API's compile-only stub (ADR-0039 step c2c2), which tools/managergen joins with it:
// View and ViewGroup fill node infos; accessibility is off (ADR-0041). ViewGroup adds its children's layout size. View
// asks for not-important views. View splits node IDs. AccessibilityRecord starts from the undefined node and item IDs.
// AccessibilityRecord names the prefetching strategy's annotation on its methods' parameters. AccessibilityRecord's
// source node, which only an accessibility service asks for, splits and makes node IDs.
import android.util.LongArray;
import android.view.View;

class AccessibilityNodeInfo {
    /** @hide */
    public static final int FLAG_SERVICE_REQUESTS_REPORT_VIEW_IDS = 1 << 8;

    /** @hide */
    public LongArray getChildNodeIds() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public long getChildId(int index) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public void addChildUnchecked(View child) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public void setExtraRenderingInfo( ExtraRenderingInfo extraRenderingInfo) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public long getSourceNodeId() {
        throw new RuntimeException("stub");
    }

    public static class ExtraRenderingInfo {
        /** @hide */
        public static ExtraRenderingInfo obtain() {
            throw new RuntimeException("stub");
        }

        /** @hide */
        public void setLayoutSize(int width, int height) {
            throw new RuntimeException("stub");
        }

        /** @hide TextView's (ADR-0043 step d)). */
        public void setTextSizeInPx(float textSizeInPx) {
            throw new RuntimeException("stub");
        }

        /** @hide TextView's (ADR-0043 step d)). */
        public void setTextSizeUnit(int textSizeUnit) {
            throw new RuntimeException("stub");
        }
    }

    /** @hide */
    public static final int FLAG_SERVICE_REQUESTS_INCLUDE_NOT_IMPORTANT_VIEWS = 1 << 7;

    /** @hide */
    public static int getVirtualDescendantId(long accessibilityNodeId) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public static final long UNDEFINED_NODE_ID = -1L;

    /** @hide */
    public @interface PrefetchingStrategy {
    }

    /** @hide */
    public static final int UNDEFINED_ITEM_ID = Integer.MAX_VALUE;

    /** @hide */
    public static int getAccessibilityViewId(long accessibilityNodeId) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public static long makeNodeId(int accessibilityViewId, int virtualDescendantId) {
        throw new RuntimeException("stub");
    }
}
