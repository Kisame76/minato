package android.database;

/**
 * Compile-only stub (ADR-0039 step c2c1), never in framework.jar: a provider's cursor for a caller of another
 * process, which ContentProviderNative's IPC makes; no other process calls a provider on Minato.
 */
public final class BulkCursorDescriptor implements android.os.Parcelable {
    public static final android.os.Parcelable.Creator<BulkCursorDescriptor> CREATOR = null;

    public android.os.IInterface cursor;

    public void writeToParcel(android.os.Parcel out, int flags) {
        throw new RuntimeException("stub");
    }

    public int describeContents() {
        throw new RuntimeException("stub");
    }
}
