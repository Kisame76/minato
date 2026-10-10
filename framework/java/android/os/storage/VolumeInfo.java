/*
 * A storage volume as vold describes it (the Unity test APK retest 11: PackageManager's hidden methods name it, and a class whose
 * method types cannot be resolved cannot be read by reflection): our class under AOSP's hidden name, without
 * members. AOSP's names vold's constants and DiskInfo; Minato has no vold, and its package manager hands no volume out
 * (ApplicationPackageManager's volume methods answer none), so no VolumeInfo is ever made.
 */
package android.os.storage;

/** @hide */
public class VolumeInfo {
    private VolumeInfo() {
    }
}
