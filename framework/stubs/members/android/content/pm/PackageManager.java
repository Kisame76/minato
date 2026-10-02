// Hidden members of the public API's compile-only stub (ADR-0039 b3.4), which tools/managergen joins with it:
// PackageItemInfo's loadIcon and loadUnbadgedIcon call them.
import android.graphics.drawable.Drawable;

class PackageManager {
    /** @hide */
    public Drawable loadItemIcon(PackageItemInfo itemInfo, ApplicationInfo appInfo) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public Drawable loadUnbadgedItemIcon(PackageItemInfo itemInfo, ApplicationInfo appInfo) {
        throw new RuntimeException("stub");
    }
}
