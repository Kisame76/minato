// Ours in place of aconfig's generated class for frameworks/av's camera_platform.aconfig (ADR-0073 step a), whose
// declarations are not fetched: each flag the ap3a release configuration sets
// (build/release's aconfig/ap3a/com.android.internal.camera.flags) with its value there, as framework.jar's other
// aconfig classes follow ap3a, and off for a flag the camera2 sources name that ap3a does not set (the declaration's
// default for a flag not yet released). Read-only, as aconfig's READ_ONLY flags are.
package com.android.internal.camera.flags;

/** @hide */
public final class Flags {
    public static final String FLAG_ANALYTICS_24Q3 = "com.android.internal.camera.flags.analytics_24q3";
    public static final String FLAG_CACHE_PERMISSION_SERVICES =
            "com.android.internal.camera.flags.cache_permission_services";
    public static final String FLAG_CALCULATE_PERF_OVERRIDE_DURING_SESSION_SUPPORT =
            "com.android.internal.camera.flags.calculate_perf_override_during_session_support";
    public static final String FLAG_CAMERA_AE_MODE_LOW_LIGHT_BOOST =
            "com.android.internal.camera.flags.camera_ae_mode_low_light_boost";
    public static final String FLAG_CAMERA_COMPAT_FOR_FREEFORM =
            "com.android.internal.camera.flags.camera_compat_for_freeform";
    public static final String FLAG_CAMERA_DEVICE_AWARENESS =
            "com.android.internal.camera.flags.camera_device_awareness";
    public static final String FLAG_CAMERA_DEVICE_SETUP = "com.android.internal.camera.flags.camera_device_setup";
    public static final String FLAG_CAMERA_EXTENSIONS_CHARACTERISTICS_GET =
            "com.android.internal.camera.flags.camera_extensions_characteristics_get";
    public static final String FLAG_CAMERA_HSUM_PERMISSION = "com.android.internal.camera.flags.camera_hsum_permission";
    public static final String FLAG_CAMERA_MANUAL_FLASH_STRENGTH_CONTROL =
            "com.android.internal.camera.flags.camera_manual_flash_strength_control";
    public static final String FLAG_CAMERA_PRIVACY_ALLOWLIST =
            "com.android.internal.camera.flags.camera_privacy_allowlist";
    public static final String FLAG_CHECK_SESSION_SUPPORT_BEFORE_SESSION_CHAR =
            "com.android.internal.camera.flags.check_session_support_before_session_char";
    public static final String FLAG_CONCERT_MODE = "com.android.internal.camera.flags.concert_mode";
    public static final String FLAG_CONCERT_MODE_API = "com.android.internal.camera.flags.concert_mode_api";
    public static final String FLAG_DELAY_LAZY_HAL_INSTANTIATION =
            "com.android.internal.camera.flags.delay_lazy_hal_instantiation";
    public static final String FLAG_EXTENSION_10_BIT = "com.android.internal.camera.flags.extension_10_bit";
    public static final String FLAG_FEATURE_COMBINATION_QUERY =
            "com.android.internal.camera.flags.feature_combination_query";
    public static final String FLAG_INJECT_SESSION_PARAMS = "com.android.internal.camera.flags.inject_session_params";
    public static final String FLAG_LAZY_AIDL_WAIT_FOR_SERVICE =
            "com.android.internal.camera.flags.lazy_aidl_wait_for_service";
    public static final String FLAG_LOG_ULTRAWIDE_USAGE = "com.android.internal.camera.flags.log_ultrawide_usage";
    public static final String FLAG_LOG_ZOOM_OVERRIDE_USAGE =
            "com.android.internal.camera.flags.log_zoom_override_usage";
    public static final String FLAG_MULTI_RES_RAW_REPROCESSING =
            "com.android.internal.camera.flags.multi_res_raw_reprocessing";
    public static final String FLAG_MULTIRESOLUTION_IMAGEREADER_USAGE_CONFIG =
            "com.android.internal.camera.flags.multiresolution_imagereader_usage_config";
    public static final String FLAG_REALTIME_PRIORITY_BUMP = "com.android.internal.camera.flags.realtime_priority_bump";
    public static final String FLAG_RETURN_BUFFERS_OUTSIDE_LOCKS =
            "com.android.internal.camera.flags.return_buffers_outside_locks";
    public static final String FLAG_SESSION_HAL_BUF_MANAGER =
            "com.android.internal.camera.flags.session_hal_buf_manager";
    public static final String FLAG_SINGLE_THREAD_EXECUTOR = "com.android.internal.camera.flags.single_thread_executor";
    public static final String FLAG_SURFACE_IPC = "com.android.internal.camera.flags.surface_ipc";
    public static final String FLAG_SURFACE_LEAK_FIX = "com.android.internal.camera.flags.surface_leak_fix";
    public static final String FLAG_USE_RO_BOARD_API_LEVEL_FOR_VNDK_VERSION =
            "com.android.internal.camera.flags.use_ro_board_api_level_for_vndk_version";
    public static final String FLAG_USE_SYSTEM_API_FOR_VNDK_VERSION =
            "com.android.internal.camera.flags.use_system_api_for_vndk_version";
    public static final String FLAG_WATCH_FOREGROUND_CHANGES =
            "com.android.internal.camera.flags.watch_foreground_changes";

    public static boolean analytics24q3() {
        return true;
    }

    public static boolean cachePermissionServices() {
        return true;
    }

    public static boolean calculatePerfOverrideDuringSessionSupport() {
        return true;
    }

    public static boolean cameraAeModeLowLightBoost() {
        return true;
    }

    public static boolean cameraCompatForFreeform() {
        return false;
    }

    public static boolean cameraDeviceAwareness() {
        return false;
    }

    public static boolean cameraDeviceSetup() {
        return true;
    }

    public static boolean cameraExtensionsCharacteristicsGet() {
        return true;
    }

    public static boolean cameraHsumPermission() {
        return true;
    }

    public static boolean cameraManualFlashStrengthControl() {
        return true;
    }

    public static boolean cameraPrivacyAllowlist() {
        return true;
    }

    public static boolean checkSessionSupportBeforeSessionChar() {
        return true;
    }

    public static boolean concertMode() {
        return true;
    }

    public static boolean concertModeApi() {
        return false;
    }

    public static boolean delayLazyHalInstantiation() {
        return true;
    }

    public static boolean extension10Bit() {
        return true;
    }

    public static boolean featureCombinationQuery() {
        return true;
    }

    public static boolean injectSessionParams() {
        return true;
    }

    public static boolean lazyAidlWaitForService() {
        return true;
    }

    public static boolean logUltrawideUsage() {
        return true;
    }

    public static boolean logZoomOverrideUsage() {
        return true;
    }

    public static boolean multiResRawReprocessing() {
        return false;
    }

    public static boolean multiresolutionImagereaderUsageConfig() {
        return true;
    }

    public static boolean realtimePriorityBump() {
        return true;
    }

    public static boolean returnBuffersOutsideLocks() {
        return true;
    }

    public static boolean sessionHalBufManager() {
        return true;
    }

    public static boolean singleThreadExecutor() {
        return true;
    }

    public static boolean surfaceIpc() {
        return true;
    }

    public static boolean surfaceLeakFix() {
        return true;
    }

    public static boolean useRoBoardApiLevelForVndkVersion() {
        return true;
    }

    public static boolean useSystemApiForVndkVersion() {
        return true;
    }

    public static boolean watchForegroundChanges() {
        return true;
    }

    private Flags() {}
}
