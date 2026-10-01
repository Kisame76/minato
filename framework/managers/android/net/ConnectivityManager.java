// The written part of ConnectivityManager (ADR-0045 decision 3), which tools/managergen joins with the API's
// methods: the hidden ctor SystemServiceRegistry calls. Decision 4's answers come with step c.
import android.content.Context;

class ConnectivityManager {
    private final Context context;

    /**
     * @hide SystemServiceRegistry's, with the context it builds the manager for. Android's ConnectivityManager takes
     * its IConnectivityManager too; ours has none.
     */
    public ConnectivityManager(Context context) {
        this.context = context;
    }
}
