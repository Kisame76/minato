// The written part of MediaRouter (services batch B), the legacy router: one system route, "Phone", that is the default
// and the selected route of every type, as the emulator (apk-runner-35) has it [verified: services-b-test]; no other
// route appears, since Minato has no cast, Bluetooth audio or display routes to offer. An app's own routes (user
// routes) are not kept.
import android.content.Context;

class MediaRouter {
    private static final RouteCategory sSystemCategory = new RouteCategory("System", ROUTE_TYPE_LIVE_AUDIO
            | ROUTE_TYPE_LIVE_VIDEO, false);
    private static final RouteInfo sDefaultRoute = new RouteInfo(sSystemCategory);

    /** @hide As the tag's, which SystemServiceRegistry calls. */
    public MediaRouter(Context context) {
    }

    public RouteInfo getDefaultRoute() {
        return sDefaultRoute;
    }

    public RouteInfo getSelectedRoute(int type) {
        return sDefaultRoute;
    }

    public int getRouteCount() {
        return 1;
    }

    public RouteInfo getRouteAt(int index) {
        if (index != 0) throw new IndexOutOfBoundsException("index " + index);
        return sDefaultRoute;
    }

    public static class RouteInfo {
        private final RouteCategory mCategory;

        /** @hide The generated subclasses (a route group, a user route) call it. */
        RouteInfo() {
            this(null);
        }

        /** @hide */
        RouteInfo(RouteCategory category) {
            mCategory = category;
        }

        public CharSequence getName() {
            return "Phone";
        }

        public RouteCategory getCategory() {
            return mCategory;
        }

        public int getSupportedTypes() {
            return ROUTE_TYPE_LIVE_AUDIO | ROUTE_TYPE_LIVE_VIDEO;
        }
    }

    public static class RouteCategory {
        private final CharSequence mName;
        private final int mTypes;
        private final boolean mGroupable;

        /** @hide */
        RouteCategory(CharSequence name, int types, boolean groupable) {
            mName = name;
            mTypes = types;
            mGroupable = groupable;
        }

        public CharSequence getName() {
            return mName;
        }

        public int getSupportedTypes() {
            return mTypes;
        }

        public boolean isGroupable() {
            return mGroupable;
        }
    }
}
