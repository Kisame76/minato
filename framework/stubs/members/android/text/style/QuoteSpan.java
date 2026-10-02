// Hidden members of the public API's compile-only stub (ADR-0039 b3.4b), which tools/managergen joins with it: AOSP's
// ParcelableSpan declares them abstract. They go when step c takes the span from AOSP.
import android.os.Parcel;

class QuoteSpan {
    /** @hide */
    public int getSpanTypeIdInternal() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public void writeToParcelInternal(Parcel dest, int flags) {
        throw new RuntimeException("stub");
    }
}
