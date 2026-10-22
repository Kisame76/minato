// Hidden members of the public API's compile-only stub (F4e), which tools/managergen joins with it: the dynamic range
// profiles' bit values, which DynamicRangeProfiles names in its constants, and the color space and stream use case
// constants of ColorSpaceProfiles and OutputConfiguration. javac inlines them, so they carry AOSP's
// values.
class CameraMetadata {
    /** @hide */
    public static final int REQUEST_AVAILABLE_DYNAMIC_RANGE_PROFILES_MAP_STANDARD = 0x1;

    /** @hide */
    public static final int REQUEST_AVAILABLE_DYNAMIC_RANGE_PROFILES_MAP_HLG10 = 0x2;

    /** @hide */
    public static final int REQUEST_AVAILABLE_DYNAMIC_RANGE_PROFILES_MAP_HDR10 = 0x4;

    /** @hide */
    public static final int REQUEST_AVAILABLE_DYNAMIC_RANGE_PROFILES_MAP_HDR10_PLUS = 0x8;

    /** @hide */
    public static final int REQUEST_AVAILABLE_DYNAMIC_RANGE_PROFILES_MAP_DOLBY_VISION_10B_HDR_REF = 0x10;

    /** @hide */
    public static final int REQUEST_AVAILABLE_DYNAMIC_RANGE_PROFILES_MAP_DOLBY_VISION_10B_HDR_REF_PO = 0x20;

    /** @hide */
    public static final int REQUEST_AVAILABLE_DYNAMIC_RANGE_PROFILES_MAP_DOLBY_VISION_10B_HDR_OEM = 0x40;

    /** @hide */
    public static final int REQUEST_AVAILABLE_DYNAMIC_RANGE_PROFILES_MAP_DOLBY_VISION_10B_HDR_OEM_PO = 0x80;

    /** @hide */
    public static final int REQUEST_AVAILABLE_DYNAMIC_RANGE_PROFILES_MAP_DOLBY_VISION_8B_HDR_REF = 0x100;

    /** @hide */
    public static final int REQUEST_AVAILABLE_DYNAMIC_RANGE_PROFILES_MAP_DOLBY_VISION_8B_HDR_REF_PO = 0x200;

    /** @hide */
    public static final int REQUEST_AVAILABLE_DYNAMIC_RANGE_PROFILES_MAP_DOLBY_VISION_8B_HDR_OEM = 0x400;

    /** @hide */
    public static final int REQUEST_AVAILABLE_DYNAMIC_RANGE_PROFILES_MAP_DOLBY_VISION_8B_HDR_OEM_PO = 0x800;

    /** @hide */
    public static final int REQUEST_AVAILABLE_DYNAMIC_RANGE_PROFILES_MAP_MAX = 0x1000;

    /** @hide */
    public static final int REQUEST_AVAILABLE_COLOR_SPACE_PROFILES_MAP_UNSPECIFIED = -1;

    /** @hide */
    public static final int SCALER_AVAILABLE_STREAM_USE_CASES_VENDOR_START = 0x10000;
}
