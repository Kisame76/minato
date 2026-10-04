// Hidden members of the public API's compile-only stub (ADR-0039 step c2c1), which tools/managergen joins with it:
// Intent's own clip data: parceling, printing and the process hops.
import android.os.Parcel;
import java.util.ArrayList;

class ClipData {
    /** @hide */
    public ClipData(CharSequence label, String[] mimeTypes, Item item) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public ClipData(ClipDescription description, Item item) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public ClipData(ClipDescription description, ArrayList<Item> items) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public ClipData(ClipData other) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public void prepareToLeaveProcess(boolean leavingPackage) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public void prepareToLeaveProcess(boolean leavingPackage, int intentFlags) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public void prepareToEnterProcess(AttributionSource source) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public void fixUris(int contentUserHint) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public void toShortString(StringBuilder b, boolean redactContent) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    ClipData(Parcel in) {
        throw new RuntimeException("stub");
    }
}
