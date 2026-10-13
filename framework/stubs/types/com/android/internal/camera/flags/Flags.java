package com.android.internal.camera.flags;

/**
 * Compile-only stub (the Unity test APK probe's gap sort, category 2), never in framework.jar: the camera's aconfig flags
 * (frameworks/av's camera_platform.aconfig, not fetched), of which CameraDevice and MultiResolutionImageReader (the
 * graphics breadth's G3a) name these in their FlaggedApi annotations; javac inlines them.
 */
public final class Flags {
    public static final String FLAG_CAMERA_DEVICE_SETUP = "com.android.internal.camera.flags.camera_device_setup";
    public static final String FLAG_MULTIRESOLUTION_IMAGEREADER_USAGE_CONFIG =
            "com.android.internal.camera.flags.multiresolution_imagereader_usage_config";
}
