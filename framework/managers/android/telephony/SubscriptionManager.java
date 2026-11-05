// The written part of SubscriptionManager (services batch B): the SIM subscriptions, of which Minato has none to show.
// What the emulator (apk-runner-35-bare, which has no modem) answers an app without READ_PHONE_STATE
// [verified: services-b-test]: the count and the list are a SecurityException, the default subscription's ID is 1
// and an ID is valid when it is more than -1.
import android.content.Context;

class SubscriptionManager {
    private final Context mContext;

    /** @hide As the tag's, which SystemServiceRegistry calls with the outer context. */
    public SubscriptionManager(Context context) {
        mContext = context;
    }

    public int getActiveSubscriptionInfoCount() {
        throw new SecurityException("getActiveSubscriptionInfoCount: The uid " + android.os.Process.myUid()
                + " does not meet the requirements to access device identifiers.");
    }

    public java.util.List<SubscriptionInfo> getActiveSubscriptionInfoList() {
        throw new SecurityException("getActiveSubscriptionInfoList: The uid " + android.os.Process.myUid()
                + " does not meet the requirements to access device identifiers.");
    }

    public static int getDefaultSubscriptionId() {
        return 1;
    }

    /**
     * The SIM slot of a subscription [verified: services-e-test]: the one subscription Minato names, ID 1, which is
     * also what the default ID (Integer.MAX_VALUE) stands for, is in slot 0; every other ID is in none.
     */
    public static int getSlotIndex(int subscriptionId) {
        return subscriptionId == getDefaultSubscriptionId() || subscriptionId == DEFAULT_SUBSCRIPTION_ID ? 0
                : INVALID_SIM_SLOT_INDEX;
    }

    public static boolean isValidSubscriptionId(int subscriptionId) {
        return subscriptionId > INVALID_SUBSCRIPTION_ID;
    }
}
