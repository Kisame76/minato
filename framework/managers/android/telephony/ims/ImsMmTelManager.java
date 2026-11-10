// The written part of ImsMmTelManager (services bundle C, part 2): the IMS manager of one subscription, for which
// ImsManager makes an instance. Its API answers are the generator's unwritten ones until an app needs more.
class ImsMmTelManager {
    private final int mSubId;

    /** @hide ImsManager's. */
    public ImsMmTelManager(int subscriptionId) {
        mSubId = subscriptionId;
    }
}
