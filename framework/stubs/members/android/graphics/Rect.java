// Hidden members of the public API's compile-only stub (ADR-0039 b3.1), which tools/managergen joins with it:
// WindowConfiguration and CompatibilityInfo call them.

import android.util.proto.ProtoInputStream;
import android.util.proto.ProtoOutputStream;
import android.util.proto.WireTypeMismatchException;
import java.io.IOException;
class Rect {
    /** @hide */
    public void dumpDebug(ProtoOutputStream protoOutputStream, long fieldId) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public void readFromProto(ProtoInputStream proto, long fieldId) throws IOException, WireTypeMismatchException {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public void scale(float scale) {
        throw new RuntimeException("stub");
    }
}
