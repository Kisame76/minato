// The written part of ProvisioningManager (services bundle C, part 2): the IMS provisioning of one subscription, for
// which ImsManager makes an instance. Its API answers are the generator's unwritten ones until an app needs more.
class ProvisioningManager {
    private final int mSubId;

    /** @hide ImsManager's. */
    public ProvisioningManager(int subscriptionId) {
        mSubId = subscriptionId;
    }
}
