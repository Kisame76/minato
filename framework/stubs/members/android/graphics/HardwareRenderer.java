// Hidden members of the public API's compile-only stub (ADR-0039 step c2c3), which tools/managergen joins with it:
// Surface's hardware canvas (lockHardwareCanvas), which only hwui's GPU pipeline gives (M4 step 10). (The framework
// breadth's batch 2) ViewDebug's picture capture, and PixelCopy's copy of a surface.

import android.view.Surface;

class HardwareRenderer {
    /** @hide */
    public void setSurface(Surface surface, boolean discardBuffer) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public float setColorMode(int colorMode) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public void setPictureCaptureCallback(PictureCapturedCallback callback) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public static void copySurfaceInto(Surface surface, CopyRequest copyRequest) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public abstract static class CopyRequest {
        protected Bitmap mDestinationBitmap;

        protected CopyRequest(Rect srcRect, Bitmap destinationBitmap) {
            throw new RuntimeException("stub");
        }

        public abstract void onCopyFinished(int result);
    }

    /** @hide */
    public interface PictureCapturedCallback {
        /** @hide */
        void onPictureCaptured(Picture picture);
    }
}
