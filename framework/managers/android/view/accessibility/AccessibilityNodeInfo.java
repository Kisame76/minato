// The written part of AccessibilityNodeInfo (the Unity test APK retest 11: a class apps reflect over names it, so it ships as
// the API with default answers), which tools/managergen joins with the API's methods. The node IDs' helpers are AOSP's
// bit arithmetic, since AccessibilityRecord and View use them without a service. Before: the hidden members of the
// public API's compile-only stub (ADR-0039 step c2c2), which tools/managergen joins with it: View and ViewGroup fill
// node infos; accessibility is off (ADR-0041). ViewGroup adds its children's layout size. View asks for not-important
// views. View splits node IDs. AccessibilityRecord starts from the undefined node and item IDs. AccessibilityRecord
// names the prefetching strategy's annotation on its methods' parameters. AccessibilityRecord's source node, which only
// an accessibility service asks for, splits and makes node IDs.
import android.util.LongArray;
import android.view.View;

class AccessibilityNodeInfo {
    /** @hide */
    public static final int FLAG_SERVICE_REQUESTS_REPORT_VIEW_IDS = 1 << 8;

    /** @hide */
    public LongArray getChildNodeIds() {
        return null;
    }

    /** @hide */
    public long getChildId(int index) {
        return 0;
    }

    /** @hide */
    public void addChildUnchecked(View child) {
    }

    /** @hide */
    public void setExtraRenderingInfo( ExtraRenderingInfo extraRenderingInfo) {
    }

    /** @hide */
    public long getSourceNodeId() {
        return 0;
    }

    public static class ExtraRenderingInfo {
        /** @hide */
        public static ExtraRenderingInfo obtain() {
            return null;
        }

        /** @hide */
        public void setLayoutSize(int width, int height) {
        }

        /** @hide TextView's (ADR-0043 step d)). */
        public void setTextSizeInPx(float textSizeInPx) {
        }

        /** @hide TextView's (ADR-0043 step d)). */
        public void setTextSizeUnit(int textSizeUnit) {
        }
    }

    /** @hide */
    public static final int FLAG_SERVICE_REQUESTS_INCLUDE_NOT_IMPORTANT_VIEWS = 1 << 7;

    /** @hide */
    public static int getVirtualDescendantId(long accessibilityNodeId) {
        return (int) ((accessibilityNodeId & 0xffffffff00000000L) >> 32);
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
        return (int) accessibilityNodeId;
    }

    /** @hide */
    public static long makeNodeId(int accessibilityViewId, int virtualDescendantId) {
        return (((long) virtualDescendantId) << 32) | accessibilityViewId;
    }
}
