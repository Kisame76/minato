// Hidden members of the public API's compile-only stub (ADR-0039 b3.1), which tools/managergen joins with it:
// Configuration, ActivityInfo, Parcelling and others call them; ParcelFileDescriptor calls readRawFileDescriptor (b3.3),
// ComponentInfo the String8 arrays (b3.4); BaseBundle and Bundle their map's and the helpers', ApplicationInfo and
// PackageInfo the squashing (b3.5a).
class Parcel {
    /** @hide */
    public final String[] readStringArray() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public final String readString8() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public final void writeString8(String val) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public final java.io.FileDescriptor readRawFileDescriptor() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public final void writeString8Array(String[] val) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public final String[] createString8Array() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public boolean hasReadWriteHelper() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public int compareData(Parcel other) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public final void adoptClassCookies(Parcel from) {
        throw new RuntimeException("stub");
    }

    /** @hide Package-private in AOSP's Parcel, which BaseBundle shares a package with. */
    void writeArrayMapInternal(android.util.ArrayMap<String, Object> val) {
        throw new RuntimeException("stub");
    }

    /** @hide Package-private in AOSP's Parcel. */
    int readArrayMap(android.util.ArrayMap<? super String, Object> map, int size, boolean sorted, boolean lazy,
            ClassLoader loader) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public final boolean pushAllowFds(boolean allowFds) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public final void restoreAllowFds(boolean lastValue) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public static boolean hasFileDescriptors(Object value) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public boolean hasBinders() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public boolean allowSquashing() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public void restoreAllowSquashing(boolean previous) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public boolean maybeWriteSquashed(Parcelable p) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public <T extends Parcelable> T readSquashed(SquashReadHelper<T> reader) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public interface SquashReadHelper<T> {
        T readRawParceled(Parcel p);
    }
}
