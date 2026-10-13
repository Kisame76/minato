package io.github.kisame76.minato.test.pad.extra;

/** The code of pad-test's on-demand feature module (ADR-0050 c2), which only a module that is installed holds. */
public class ExtraClass {
    public static String answer() {
        return "the on-demand module answers";
    }

    /** The module's own JNI library, which its arm64 split holds (ADR-0050 c3). */
    public static String library() {
        try {
            System.loadLibrary("extra_native");
            return nativeAnswer();
        } catch (Throwable t) {
            return t.getClass().getName();
        }
    }

    static native String nativeAnswer();
}
