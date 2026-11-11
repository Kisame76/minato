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

    // The flags and the type the route button and its dialogs name; AOSP's values.
    /** @hide */
    public static final int CALLBACK_FLAG_PASSIVE_DISCOVERY = 1 << 3;
    /** @hide */
    public static final int AVAILABILITY_FLAG_IGNORE_DEFAULT_ROUTE = 1 << 0;
    /** @hide */
    public static final int ROUTE_TYPE_REMOTE_DISPLAY = 1 << 2;

    /** @hide The route of every type, here the default one. */
    public RouteInfo getSelectedRoute() {
        return sDefaultRoute;
    }

    /** @hide As AOSP's: whether a route of the types is there, without the default route when the flag says so. */
    public boolean isRouteAvailable(int types, int flags) {
        if ((flags & AVAILABILITY_FLAG_IGNORE_DEFAULT_ROUTE) != 0) return false;
        return sDefaultRoute.matchesTypes(types);
    }

    /** @hide No other route exists, so the fallback is the default route. */
    public RouteInfo getFallbackRoute() {
        return sDefaultRoute;
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

        /** @hide The route is the default one; it is selected for every type, and no Bluetooth route exists. */
        public boolean isDefault() {
            return this == sDefaultRoute;
        }

        /** @hide */
        public boolean isSelected() {
            return this == sDefaultRoute;
        }

        /** @hide */
        public boolean isBluetooth() {
            return false;
        }

        /** @hide */
        public boolean matchesTypes(int types) {
            return (getSupportedTypes() & types) != 0;
        }

        /** @hide Selecting the route that is already the only one changes nothing. */
        public void select() {
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
