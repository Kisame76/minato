// The written part of ConnectivityDiagnosticsManager (service inventory, bundle C part 1): connectivity diagnostics: the manager is there; no callback is told anything on a Minato network [verified: services-j-test].
import android.content.Context;

class ConnectivityDiagnosticsManager {
    private final Context mContext;

    /** @hide As the tag's, which SystemServiceRegistry calls with the outer context. */
    public ConnectivityDiagnosticsManager(Context context) {
        mContext = context;
    }
}
