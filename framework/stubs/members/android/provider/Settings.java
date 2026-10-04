// Hidden members of the public API's compile-only stub (ADR-0039 step c2c2), which tools/managergen joins with it:
// ViewConfiguration's settings names; javac inlines them, so they carry AOSP's values.
class Settings {
    public static class Secure {
        /** @hide */
        public static final String LONG_PRESS_TIMEOUT = "long_press_timeout";

        /** @hide */
        public static final String MULTI_PRESS_TIMEOUT = "multi_press_timeout";

        /** @hide */
        public static final String KEY_REPEAT_TIMEOUT_MS = "key_repeat_timeout";

        /** @hide */
        public static final String KEY_REPEAT_DELAY_MS = "key_repeat_delay";
    }
}
