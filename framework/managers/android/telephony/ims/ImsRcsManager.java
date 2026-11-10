// The written part of ImsRcsManager (services bundle C, part 2): the IMS RCS manager of one subscription, for which
// ImsManager makes an instance. Its API answers are the generator's unwritten ones until an app needs more.
class ImsRcsManager {
    private final int mSubId;

    /** @hide ImsManager's. */
    public ImsRcsManager(int subscriptionId) {
        mSubId = subscriptionId;
    }
}
