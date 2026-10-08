package android.app.admin;

/**
 * Compile-only stub (ADR-0045 gap 1, batch 1b), never in framework.jar: the keys of the device policy's updatable
 * resources, which UserManager's badges and labels of a work profile look up through DevicePolicyManager; javac
 * inlines them, with their values at android-15.0.0_r1. Minato has no work profile, so no lookup runs.
 */
public final class DevicePolicyResources {
    public static final String UNDEFINED = "UNDEFINED";

    public static final class Drawables {
        public static final String WORK_PROFILE_ICON_BADGE = "WORK_PROFILE_ICON_BADGE";

        public static final class Style {
            public static final String SOLID_COLORED = "SOLID_COLORED";
        }
    }

    public static final class Strings {
        public static final class SystemUi {
            public static final String STATUS_BAR_WORK_ICON_ACCESSIBILITY =
                    "SystemUi.STATUS_BAR_WORK_ICON_ACCESSIBILITY";
        }

        public static final class Core {
            public static final String RESOLVER_WORK_TAB = "Core.RESOLVER_WORK_TAB";

            public static final String WORK_PROFILE_BADGED_LABEL = "Core.WORK_PROFILE_BADGED_LABEL";
        }
    }
}
