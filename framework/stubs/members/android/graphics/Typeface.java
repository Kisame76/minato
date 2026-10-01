// Hidden members of the public API's compile-only stub (ADR-0039 b3.1), which tools/managergen joins with it:
// ResourcesImpl's fonts call them.
import android.content.res.AssetManager;
import android.content.res.FontResourcesParser.FamilyResourceEntry;

class Typeface {
    public static final class Builder {
        /** @hide */
        public Builder(AssetManager assetManager, String path, boolean isAsset, int cookie) {
            throw new RuntimeException("stub");
        }
    }

    /** @hide */
    public static final int RESOLVE_BY_FONT_TABLE = -1;

    /** @hide */
    public static Typeface findFromCache(AssetManager mgr, String path) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public static Typeface createFromResources(FamilyResourceEntry entry, AssetManager mgr, String path) {
        throw new RuntimeException("stub");
    }
}
