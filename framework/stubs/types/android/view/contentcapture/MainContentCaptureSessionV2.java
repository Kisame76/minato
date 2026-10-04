package android.view.contentcapture;

/**
 * Compile-only stub (ADR-0039 step c2c1), never in framework.jar: ContentCaptureManager's main session; content
 * capture stays off on Minato: no Context gives a ContentCaptureManager, as on a device without the service.
 */
public final class MainContentCaptureSessionV2 extends ContentCaptureSession {
    public MainContentCaptureSessionV2(ContentCaptureManager.StrippedContext context, ContentCaptureManager manager, android.os.Handler uiHandler, android.os.Handler contentCaptureHandler, IContentCaptureManager systemServerInterface) {
        throw new RuntimeException("stub");
    }
}
