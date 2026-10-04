// Hidden members of the public API's compile-only stub (ADR-0039 step c2c1), which tools/managergen joins with it:
// ContentProvider's transport rewrites an operation for its user.
import android.net.Uri;
import android.os.Parcel;

class ContentProviderOperation {
    /** @hide */
    public ContentProviderOperation(ContentProviderOperation cpo, Uri withUri) {
        throw new RuntimeException("stub");
    }
}
