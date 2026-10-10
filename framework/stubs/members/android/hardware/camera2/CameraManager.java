// Hidden members of the public API's compile-only stub (the Unity test APK probe's batch C), which tools/managergen joins
// with it: the rotation override that the legacy Camera's open(int) and getCameraInfo ask for a camera's ID.
class CameraManager {
    /** @hide */
    public static int getRotationOverride(android.content.Context context) {
        throw new RuntimeException("stub");
    }
}
