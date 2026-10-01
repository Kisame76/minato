// Hidden members of the public API's compile-only stub (ADR-0039 b3.1), which tools/managergen joins with it:
// CompatibilityInfo.Translator calls them on LayoutParams.
class WindowManager {
    public static class LayoutParams {
        /** @hide */
        public void scale(float scale) {
            throw new RuntimeException("stub");
        }

        /** @hide */
        public @interface SoftInputModeFlags {
        }
    }
}
