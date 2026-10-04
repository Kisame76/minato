// Hidden members of the public API's compile-only stub (ADR-0039 step c2c2), which tools/managergen joins with it:
// View's autofill ID. View's virtual autofill IDs.
class AutofillId {
    /** @hide */
    public AutofillId(int id) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public int getViewId() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public boolean isNonVirtual() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public AutofillId(AutofillId hostId, int virtualChildId) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public AutofillId(int hostId, int virtualChildId) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public boolean isVirtualInt() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public int getVirtualChildIntId() {
        throw new RuntimeException("stub");
    }
}
