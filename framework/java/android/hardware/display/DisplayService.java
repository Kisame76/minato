/*
 * The display service (ADR-0041 decision 6; ADR-0045 decisions 2 and 3): the IDisplayManager of display 0, local in the
 * app's process, which ServiceManager's table gives as "display". It is AIDL's Default class with Minato's answers, not
 * a ServiceStub proxy: IDisplayManager names types that framework.jar lacks, and ART resolves every type in the
 * signatures of a proxy's interface (libcore's Class.getDeclaredMethods), while a Default method that names one only
 * matters when it runs. Its answers:
 *   - getDisplayInfo(0): the session's screen, which ActivityThread put into the system Resources (ADR-0028's
 *     configuration), at the session's highest refresh rate, at which Minato's frame clock runs (ADR-0036); modes of
 *     60 Hz and that rate, all of them for apps too (appsSupportedModes, which Display.getSupportedModes answers);
 *     a built-in screen's flags; rotation 90 for a landscape session; no other display;
 *   - getDisplayIds: display 0 alone;
 *   - getPreferredWideGamutColorSpaceId: Display P3, an iPhone's panel;
 *   - createVirtualDisplay: no display, so that DisplayManagerGlobal's wrapper fails as Android's does without one;
 *   - the 25 methods whose permission IDisplayManager.Stub enforces throw SecurityException with Android's message:
 *     each is a signature permission that an app never holds (ADR-0045 decision 6).
 * The rest returns AIDL's defaults, without decision 3's once-per-method log line, and registerCallback keeps nothing:
 * Minato's display changes only at a turn (turn). Not under com.android.minato, whose classes
 * tools/framework-host-check.sh compiles on the host without android.*.
 */
package android.hardware.display;

import android.Manifest;
import android.content.pm.ParceledListSlice;
import android.content.res.Resources;
import android.graphics.ColorSpace;
import android.media.projection.IMediaProjection;
import android.os.Binder;
import android.os.IBinder;
import android.util.DisplayMetrics;
import android.view.Display;
import android.view.DisplayInfo;
import android.view.Surface;

public final class DisplayService extends IDisplayManager.Default {
    /** PermissionEnforcer's message. */
    private static final String ACCESS_DENIED = "Access denied, requires: ";
    /** The rate without a session, as the emulator's display has it. */
    private static final int RATE_WITHOUT_SESSION = 60;

    /** The one service of the process, which a turn changes (ActivityThread's, at the session's turn). */
    private static DisplayService sInstance;

    private final Binder mBinder = new Binder();
    private volatile DisplayInfo mInfo;

    private DisplayService(DisplayInfo info) {
        mInfo = info;
        mBinder.attachInterface(this, IDisplayManager.Stub.DESCRIPTOR);
    }

    /** ServiceManager's maker for "display": the Binder that carries a new service. */
    public static IBinder newBinder() {
        DisplayService service = new DisplayService(sessionDisplay());
        sInstance = service;
        return service.asBinder();
    }

    /**
     * The screen turned (ADR-0038: the window covers it whole): display 0 now has this size, at the density of the
     * system Resources, and its rotation follows; the display manager's cache and listeners are told by the caller.
     */
    public static void turn(int width, int height) {
        DisplayService service = sInstance;
        if (service != null) {
            service.mInfo = display(width, height, Resources.getSystem().getDisplayMetrics().densityDpi);
        }
    }

    @Override
    public IBinder asBinder() {
        return mBinder;
    }

    /** A copy for each call, which the caller may change, as each of Android's comes from a parcel. */
    @Override
    public DisplayInfo getDisplayInfo(int displayId) {
        return displayId == Display.DEFAULT_DISPLAY ? new DisplayInfo(mInfo) : null;
    }

    @Override
    public int[] getDisplayIds(boolean includeDisabled) {
        return new int[] {Display.DEFAULT_DISPLAY};
    }

    @Override
    public int getPreferredWideGamutColorSpaceId() {
        return ColorSpace.Named.DISPLAY_P3.ordinal();
    }

    /** Minato makes no virtual display: DisplayManagerGlobal logs it and gives null, as for a failed creation. */
    @Override
    public int createVirtualDisplay(VirtualDisplayConfig virtualDisplayConfig, IVirtualDisplayCallback callback,
            IMediaProjection projectionToken, String packageName) {
        return Display.INVALID_DISPLAY;
    }

    @Override
    public void startWifiDisplayScan() {
        throw denied(Manifest.permission.CONFIGURE_WIFI_DISPLAY);
    }

    @Override
    public void stopWifiDisplayScan() {
        throw denied(Manifest.permission.CONFIGURE_WIFI_DISPLAY);
    }

    @Override
    public void pauseWifiDisplay() {
        throw denied(Manifest.permission.CONFIGURE_WIFI_DISPLAY);
    }

    @Override
    public void resumeWifiDisplay() {
        throw denied(Manifest.permission.CONFIGURE_WIFI_DISPLAY);
    }

    @Override
    public void setUserDisabledHdrTypes(int[] userDisabledTypes) {
        throw denied(Manifest.permission.WRITE_SECURE_SETTINGS);
    }

    @Override
    public void setAreUserDisabledHdrTypesAllowed(boolean areUserDisabledHdrTypesAllowed) {
        throw denied(Manifest.permission.WRITE_SECURE_SETTINGS);
    }

    @Override
    public void requestColorMode(int displayId, int colorMode) {
        throw denied(Manifest.permission.CONFIGURE_DISPLAY_COLOR_MODE);
    }

    @Override
    public ParceledListSlice<?> getBrightnessEvents(String callingPackage) {
        throw denied(Manifest.permission.BRIGHTNESS_SLIDER_USAGE);
    }

    @Override
    public ParceledListSlice<?> getAmbientBrightnessStats() {
        throw denied(Manifest.permission.ACCESS_AMBIENT_LIGHT_STATS);
    }

    @Override
    public void setBrightnessConfigurationForUser(BrightnessConfiguration c, int userId, String packageName) {
        throw denied(Manifest.permission.CONFIGURE_DISPLAY_BRIGHTNESS);
    }

    @Override
    public void setBrightnessConfigurationForDisplay(BrightnessConfiguration c, String uniqueDisplayId, int userId,
            String packageName) {
        throw denied(Manifest.permission.CONFIGURE_DISPLAY_BRIGHTNESS);
    }

    @Override
    public BrightnessConfiguration getBrightnessConfigurationForDisplay(String uniqueDisplayId, int userId) {
        throw denied(Manifest.permission.CONFIGURE_DISPLAY_BRIGHTNESS);
    }

    @Override
    public BrightnessConfiguration getDefaultBrightnessConfiguration() {
        throw denied(Manifest.permission.CONFIGURE_DISPLAY_BRIGHTNESS);
    }

    @Override
    public void setTemporaryBrightness(int displayId, float brightness) {
        throw denied(Manifest.permission.CONTROL_DISPLAY_BRIGHTNESS);
    }

    @Override
    public void setBrightness(int displayId, float brightness) {
        throw denied(Manifest.permission.CONTROL_DISPLAY_BRIGHTNESS);
    }

    @Override
    public void setTemporaryAutoBrightnessAdjustment(float adjustment) {
        throw denied(Manifest.permission.CONTROL_DISPLAY_BRIGHTNESS);
    }

    @Override
    public BrightnessInfo getBrightnessInfo(int displayId) {
        throw denied(Manifest.permission.CONTROL_DISPLAY_BRIGHTNESS);
    }

    @Override
    public void setUserPreferredDisplayMode(int displayId, Display.Mode mode) {
        throw denied(Manifest.permission.MODIFY_USER_PREFERRED_DISPLAY_MODE);
    }

    @Override
    public void setShouldAlwaysRespectAppRequestedMode(boolean enabled) {
        throw denied(Manifest.permission.OVERRIDE_DISPLAY_MODE_REQUESTS);
    }

    @Override
    public boolean shouldAlwaysRespectAppRequestedMode() {
        throw denied(Manifest.permission.OVERRIDE_DISPLAY_MODE_REQUESTS);
    }

    @Override
    public void setRefreshRateSwitchingType(int newValue) {
        throw denied(Manifest.permission.MODIFY_REFRESH_RATE_SWITCHING_TYPE);
    }

    @Override
    public void enableConnectedDisplay(int displayId) {
        throw denied(Manifest.permission.MANAGE_DISPLAYS);
    }

    @Override
    public void disableConnectedDisplay(int displayId) {
        throw denied(Manifest.permission.MANAGE_DISPLAYS);
    }

    @Override
    public boolean requestDisplayPower(int displayId, boolean on) {
        throw denied(Manifest.permission.MANAGE_DISPLAYS);
    }

    @Override
    public void requestDisplayModes(IBinder token, int displayId, int[] modeIds) {
        throw denied(Manifest.permission.RESTRICT_DISPLAY_MODES);
    }

    private static SecurityException denied(String permission) {
        return new SecurityException(ACCESS_DENIED + permission);
    }

    /**
     * Display 0 as LocalDisplayAdapter and LogicalDisplay describe a phone's built-in screen: the system Resources'
     * size and density, which the window covers whole (ADR-0038), and the session's rate, or 60 Hz without a session.
     * A mode's size is the screen's natural one, in portrait.
     */
    private static DisplayInfo sessionDisplay() {
        DisplayMetrics metrics = Resources.getSystem().getDisplayMetrics();
        return display(metrics.widthPixels, metrics.heightPixels, metrics.densityDpi);
    }

    private static DisplayInfo display(int width, int height, int densityDpi) {
        int rate = nativeSessionRefreshRate();
        if (rate <= 0) {
            rate = RATE_WITHOUT_SESSION;
        }
        DisplayInfo info = new DisplayInfo();
        info.type = Display.TYPE_INTERNAL;
        // A built-in screen's flags, as the emulator's display 0 has them (0x4083): it turns with its content.
        info.flags = Display.FLAG_SUPPORTS_PROTECTED_BUFFERS | Display.FLAG_SECURE | Display.FLAG_TRUSTED
                | Display.FLAG_ROTATES_WITH_CONTENT;
        info.displayId = Display.DEFAULT_DISPLAY;
        info.displayGroupId = Display.DEFAULT_DISPLAY_GROUP;
        info.name = "Built-in Screen";
        info.uniqueId = "local:0";
        info.logicalWidth = info.appWidth = width;
        info.logicalHeight = info.appHeight = height;
        info.smallestNominalAppWidth = info.smallestNominalAppHeight = Math.min(width, height);
        info.largestNominalAppWidth = info.largestNominalAppHeight = Math.max(width, height);
        info.rotation = width > height ? Surface.ROTATION_90 : Surface.ROTATION_0;
        int naturalWidth = Math.min(width, height);
        int naturalHeight = Math.max(width, height);
        Display.Mode sixty = new Display.Mode(1, naturalWidth, naturalHeight, RATE_WITHOUT_SESSION);
        info.supportedModes = rate == RATE_WITHOUT_SESSION ? new Display.Mode[] {sixty}
                : new Display.Mode[] {sixty, new Display.Mode(2, naturalWidth, naturalHeight, rate)};
        // Display.getSupportedModes answers the modes apps may see, every one of them here.
        info.appsSupportedModes = info.supportedModes;
        info.modeId = info.defaultModeId = info.supportedModes[info.supportedModes.length - 1].getModeId();
        info.renderFrameRate = rate;
        info.logicalDensityDpi = densityDpi;
        info.physicalXDpi = info.physicalYDpi = densityDpi;
        info.presentationDeadlineNanos = 1_000_000_000L / rate;
        info.state = info.committedState = Display.STATE_ON;
        return info;
    }

    /** The session's highest refresh rate (ADR-0028's configuration), 0 without a session. */
    private static native int nativeSessionRefreshRate();
}
