// The written part of HealthConnectManager (service inventory, bundle B rest): the manager is there; an app without a
// health permission is refused as Android refuses it [verified: services-h-test].
import android.content.Context;

class HealthConnectManager {
    private final Context mContext;

    /** @hide As the tag's, which SystemServiceRegistry calls with the outer context. */
    public HealthConnectManager(Context context) {
        mContext = context;
    }

    /** @hide A hidden getter of the tag's: it needs a health permission. */
    public int getRecordRetentionPeriodInDays() {
        throw new SecurityException("Caller does not have a health permission");
    }
}
