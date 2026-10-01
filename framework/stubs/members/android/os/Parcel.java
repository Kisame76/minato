// Hidden members of the public API's compile-only stub (ADR-0039 b3.1), which tools/managergen joins with it:
// Configuration, ActivityInfo, Parcelling and others call them.
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
}
