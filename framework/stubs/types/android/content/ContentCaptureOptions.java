package android.content;

/**
 * Compile-only stub (ADR-0039 step c1), never in framework.jar: Context's hidden getContentCaptureOptions and
 * setContentCaptureOptions name it, and ContentCaptureManager reads its fields (step c2c1), which are not final so
 * that javac reads them instead of inlining a stub's value; content capture is off in Minato (ADR-0041).
 */
public final class ContentCaptureOptions {
    public int loggingLevel;

    public boolean disableFlushForViewTreeAppearing;

    public ContentProtectionOptions contentProtectionOptions;

    public boolean lite;

    public void dumpShort(java.io.PrintWriter pw) {
        throw new RuntimeException("stub");
    }

    public static final class ContentProtectionOptions {
        public boolean enableReceiver;

        public int bufferSize;
    }
}
