// Hidden members of the public API's compile-only stub (ADR-0039 step c2c3), which tools/managergen joins with it:
// PhoneWindow's assist key.
// (ADR-0039 step c2c3) Activity's search manager.
import android.os.Bundle;
import android.content.Context;
import android.os.Handler;
import android.os.ServiceManager.ServiceNotFoundException;

class SearchManager {
    /** @hide */
    public void launchAssist(Bundle args) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    SearchManager(Context context, Handler handler) throws ServiceNotFoundException {
        throw new RuntimeException("stub");
    }
}
