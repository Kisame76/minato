// The written part of WifiManager (ADR-0045 decision 3), which tools/managergen joins with the API's methods: the
// hidden ctor SystemServiceRegistry calls. Decision 4's answers come with step c.
import android.content.Context;

class WifiManager {
    private final Context context;

    /**
     * @hide SystemServiceRegistry's, with the context it builds the manager for. Android's WifiManager takes its
     * IWifiManager too; ours has none.
     */
    public WifiManager(Context context) {
        this.context = context;
    }
}
