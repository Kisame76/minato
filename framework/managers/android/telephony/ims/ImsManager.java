// The written part of ImsManager (services bundle C, part 2 of the system services gap): the IMS managers of a
// subscription. Minato has no modem, so the managers are there and answer as one without IMS would; the emulator
// (apk-runner-35-bare) hands out both for a subscription ID [verified: services-i-test]. A subscription ID that is not
// valid is an IllegalArgumentException, as AOSP's [inferred from android-15.0.0_r1, ImsManager.java].
import android.content.Context;
import android.telephony.SubscriptionManager;

class ImsManager {
    /** @hide As the tag's, which SystemServiceRegistry calls with the outer context. */
    public ImsManager(Context context) {
    }

    public ImsMmTelManager getImsMmTelManager(int subscriptionId) {
        if (!SubscriptionManager.isValidSubscriptionId(subscriptionId)) {
            throw new IllegalArgumentException("Invalid subscription ID: " + subscriptionId);
        }
        return new ImsMmTelManager(subscriptionId);
    }

    public ImsRcsManager getImsRcsManager(int subscriptionId) {
        if (!SubscriptionManager.isValidSubscriptionId(subscriptionId)) {
            throw new IllegalArgumentException("Invalid subscription ID: " + subscriptionId);
        }
        return new ImsRcsManager(subscriptionId);
    }

    public ProvisioningManager getProvisioningManager(int subscriptionId) {
        if (!SubscriptionManager.isValidSubscriptionId(subscriptionId)) {
            throw new IllegalArgumentException("Invalid subscription ID: " + subscriptionId);
        }
        return new ProvisioningManager(subscriptionId);
    }
}
