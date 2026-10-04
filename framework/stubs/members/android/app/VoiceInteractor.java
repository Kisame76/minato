// Hidden members of the public API's compile-only stub (ADR-0039 step c2c3), which tools/managergen joins with it:
// Activity's voice interactor. (ADR-0039 step c2c3) Activity clears its voice requests. (ADR-0039 step c2c3) Activity
// makes its voice interactor.
import java.io.FileDescriptor;
import java.io.PrintWriter;
import android.content.Context;
import android.os.Looper;
import com.android.internal.app.IVoiceInteractor;

class VoiceInteractor {
    /** @hide */
    void attachActivity(Activity activity) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    void retainInstance() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    void detachActivity() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    void dump(String prefix, FileDescriptor fd, PrintWriter writer, String[] args) {
        throw new RuntimeException("stub");
    }

    public static class Request {
        /** @hide */
        void clear() {
            throw new RuntimeException("stub");
        }
    }

    /** @hide */
    VoiceInteractor(IVoiceInteractor interactor, Context context, Activity activity, Looper looper) {
        throw new RuntimeException("stub");
    }
}
