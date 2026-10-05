// Hidden members of the public API's compile-only stub (ADR-0045, gap 13a), which tools/managergen joins with it:
// IContentService's AIDL code reads sync infos through their creator; the local content service never runs it.
import android.os.Parcelable;

class SyncInfo {
    /** @hide */
    public static final Parcelable.Creator<SyncInfo> CREATOR = null;
}
