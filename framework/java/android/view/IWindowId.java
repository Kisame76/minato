package android.view;

/**
 * Placeholder (the Unity test APK retest 11; replaced by ADR-0058 b) / category 2): our class under AOSP's
 * hidden name, which only has to load, since a class apps reflect over names it; every body logs once and throws
 * UnsupportedOperationException. Before, a compile-only stub (ADR-0039 step c2c2): AttachInfo's window ID for
 * View.getWindowId.
 */
public interface IWindowId extends android.os.IInterface {
}
