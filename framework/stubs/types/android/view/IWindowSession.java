package android.view;

/**
 * Compile-only stub (ADR-0039 step c2c2), never in framework.jar: the window session that AttachInfo keeps and View's
 * drag and drop, moving and display hash call; ViewRootImpl's world.
 */
public interface IWindowSession extends android.os.IInterface {
    void cancelDragAndDrop(android.os.IBinder dragToken, boolean skipAnimation) throws android.os.RemoteException;

    boolean dropForAccessibility(IWindow window, int x, int y) throws android.os.RemoteException;

    void finishMovingTask(IWindow window) throws android.os.RemoteException;

    IWindowId getWindowId(android.os.IBinder window) throws android.os.RemoteException;

    boolean startMovingTask(IWindow window, float startX, float startY) throws android.os.RemoteException;

    void generateDisplayHash(IWindow window, android.graphics.Rect boundsInWindow, String hashAlgorithm,
            android.os.RemoteCallback callback) throws android.os.RemoteException;

    android.os.IBinder performDrag(IWindow window, int flags, SurfaceControl surface, int touchSource, int touchDeviceId,
            int touchPointerId, float touchX, float touchY, float thumbCenterX, float thumbCenterY,
            android.content.ClipData data) throws android.os.RemoteException;

    void setOnBackInvokedCallbackInfo(IWindow window, android.window.OnBackInvokedCallbackInfo callbackInfo)
            throws android.os.RemoteException;
}
