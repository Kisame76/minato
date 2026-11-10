// The written part of NetworkStatsManager (services bundle A of the system services gap): the data usage of the device,
// of a user and of an app. Every query needs PACKAGE_USAGE_STATS or READ_NETWORK_USAGE_HISTORY, which an app does not
// hold: SecurityException, as the emulator (apk-runner-35-bare) answers [verified: services-f-test].
import android.content.Context;

class NetworkStatsManager {
    /** @hide As the tag's, which SystemServiceRegistry calls. */
    public NetworkStatsManager(Context context) {
    }

    private static SecurityException notAllowed() {
        return new SecurityException("Network stats history of uid " + android.os.Process.myUid()
                + " is forbidden for caller " + android.os.Process.myUid());
    }

    public NetworkStats.Bucket querySummaryForDevice(int networkType, String subscriberId, long startTime,
            long endTime) {
        throw notAllowed();
    }

    public NetworkStats.Bucket querySummaryForUser(int networkType, String subscriberId, long startTime,
            long endTime) {
        throw notAllowed();
    }

    public NetworkStats querySummary(int networkType, String subscriberId, long startTime, long endTime) {
        throw notAllowed();
    }

    public NetworkStats queryDetailsForUid(int networkType, String subscriberId, long startTime, long endTime,
            int uid) {
        throw notAllowed();
    }

    public NetworkStats queryDetailsForUidTag(int networkType, String subscriberId, long startTime, long endTime,
            int uid, int tag) {
        throw notAllowed();
    }

    public NetworkStats queryDetailsForUidTagState(int networkType, String subscriberId, long startTime,
            long endTime, int uid, int tag, int state) {
        throw notAllowed();
    }

    public NetworkStats queryDetails(int networkType, String subscriberId, long startTime, long endTime) {
        throw notAllowed();
    }
}
