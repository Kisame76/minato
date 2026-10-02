// Hidden members of the public API's compile-only stub (ADR-0039 b3.1), which tools/managergen joins with it:
// Configuration, ActivityInfo, Parcelling and others call them; ParcelFileDescriptor calls readRawFileDescriptor (b3.3),
// ComponentInfo the String8 arrays (b3.4).
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
}
