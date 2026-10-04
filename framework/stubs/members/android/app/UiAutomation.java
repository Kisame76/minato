// Hidden members of the public API's compile-only stub (ADR-0039 step c2c3), which tools/managergen joins with it:
// Instrumentation's UiAutomation.
import android.content.Context;
import android.os.Looper;
import java.util.concurrent.TimeoutException;

class UiAutomation {
    /** @hide */
    public UiAutomation(Context context, IUiAutomationConnection connection) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public UiAutomation(Looper looper, IUiAutomationConnection connection) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public void connect() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public void connect(int flags) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public void connectWithTimeout(int flags, long timeoutMillis) throws TimeoutException {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public int getFlags() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public void disconnect() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public boolean isDestroyed() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public void destroy() {
        throw new RuntimeException("stub");
    }
}
