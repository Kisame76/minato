// Hidden members of the public API's compile-only stub (ADR-0039 b3.1), which tools/managergen joins with it:
// ResourcesImpl's drawables call them.
import android.content.res.AssetManager.AssetInputStream;
import android.content.res.Resources;
import android.util.TypedValue;
import java.io.InputStream;

class ImageDecoder {
    /** @hide */
    public static class AssetInputStreamSource extends Source {
        public AssetInputStreamSource(AssetInputStream ais, Resources res, TypedValue value) {
            throw new RuntimeException("stub");
        }
    }

    /** @hide */
    public static Source createSource(Resources res, InputStream is) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public static Source createSource(Resources res, InputStream is, int density) {
        throw new RuntimeException("stub");
    }
}
