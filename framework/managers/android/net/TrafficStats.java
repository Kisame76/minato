// The written part of TrafficStats (ADR-0045 decision 3; the Unity test APK probe's blocker 9, Firebase Installations'), which
// tools/managergen joins with the API's methods: Android 15's TrafficStats (the Connectivity module's framework-t at
// the tag) without its statistics service. Android's are the thread's tag and uid, the socket tagger that
// ActivityThread.main attaches as RuntimeInit.commonInit does, with its log lines and StrictMode's untagged socket
// check, and the hidden constants. The kernel's tag is left out, as nothing on Minato counts by tag and uid (Android's
// BPF maps). Of the counters that NetworkStatsService gives, another uid's and the transport layer's are UNSUPPORTED,
// as on Android. The app's own, the totals and an interface's are UNSUPPORTED too, Android's answer for a device that
// does not count: a named difference until a later step counts the app's socket bytes in the runtime. Minato has
// no mobile interface, so the mobile sums are 0. incrementOperationCount checks as NetworkStatsService does and keeps
// nothing.
import android.content.Context;
import android.os.StrictMode;
import android.util.Log;

import java.io.FileDescriptor;
import java.io.IOException;
import java.net.DatagramSocket;
import java.net.Socket;
import java.net.SocketException;
import java.util.Objects;

class TrafficStats {
    private static final String TAG = "TrafficStats";

    /** @hide */
    @Deprecated
    public static final long KB_IN_BYTES = 1024;
    /** @hide */
    @Deprecated
    public static final long MB_IN_BYTES = KB_IN_BYTES * 1024;
    /** @hide */
    @Deprecated
    public static final long GB_IN_BYTES = MB_IN_BYTES * 1024;
    /** @hide */
    @Deprecated
    public static final long TB_IN_BYTES = GB_IN_BYTES * 1024;
    /** @hide */
    @Deprecated
    public static final long PB_IN_BYTES = TB_IN_BYTES * 1024;

    /** @hide The uid of an app that was removed, in NetworkStats. */
    public static final int UID_REMOVED = -4;
    /** @hide NetworkStats.UID_TETHERING, the uid of tethering's traffic. */
    public static final int UID_TETHERING = -5;

    /** @hide */
    public static final int TAG_NETWORK_STACK_RANGE_START = 0xFFFFFD00;
    /** @hide */
    public static final int TAG_NETWORK_STACK_RANGE_END = 0xFFFFFEFF;
    /** @hide */
    public static final int TAG_SYSTEM_IMPERSONATION_RANGE_START = 0xFFFFFF00;
    /** @hide */
    public static final int TAG_SYSTEM_IMPERSONATION_RANGE_END = 0xFFFFFF0F;
    /** @hide */
    public static final int TAG_NETWORK_STACK_IMPERSONATION_RANGE_START = 0xFFFFFF80;
    /** @hide */
    public static final int TAG_NETWORK_STACK_IMPERSONATION_RANGE_END = 0xFFFFFF8F;
    /** @hide DownloadManager's traffic. */
    public static final int TAG_SYSTEM_DOWNLOAD = 0xFFFFFF01;
    /** @hide MediaPlayer's traffic. */
    public static final int TAG_SYSTEM_MEDIA = 0xFFFFFF02;
    /** @hide BackupManager's backups. */
    public static final int TAG_SYSTEM_BACKUP = 0xFFFFFF03;
    /** @hide BackupManager's restores. */
    public static final int TAG_SYSTEM_RESTORE = 0xFFFFFF04;
    /** @hide An app's own traffic that the system makes for it. */
    public static final int TAG_SYSTEM_APP = 0xFFFFFF05;
    /** @hide */
    public static final int TAG_SYSTEM_PROBE = 0xFFFFFF42;

    /** @hide The counters' types, as NetworkStatsService's natives number them. */
    public static final int TYPE_RX_BYTES = 0;
    /** @hide */
    public static final int TYPE_RX_PACKETS = 1;
    /** @hide */
    public static final int TYPE_TX_BYTES = 2;
    /** @hide */
    public static final int TYPE_TX_PACKETS = 3;

    public TrafficStats() {
    }

    /**
     * @hide ActivityThread.handleBindApplication's on Android, which hands over NetworkStatsManager's binder. Minato
     * has no statistics service, so there is nothing to keep.
     */
    public static void init(Context context) {
    }

    /** @hide RuntimeInit.commonInit's on Android, ActivityThread.main's on Minato: every new socket gets tagged. */
    public static void attachSocketTagger() {
        dalvik.system.SocketTagger.set(new SocketTagger());
    }

    private static class SocketTagger extends dalvik.system.SocketTagger {
        // Android 15 logs every tag and untag (its "TODO: set to false").
        private static final boolean LOGD = true;

        SocketTagger() {
        }

        @Override
        public void tag(FileDescriptor fd) throws SocketException {
            final UidTag tagInfo = sThreadUidTag.get();
            if (LOGD) {
                Log.d(TAG, "tagSocket(" + fd.getInt$() + ") with statsTag=0x"
                        + Integer.toHexString(tagInfo.tag) + ", statsUid=" + tagInfo.uid);
            }
            if (tagInfo.tag == -1) {
                StrictMode.noteUntaggedSocket();
            }
            // Android tags the socket in the kernel here (native_tagSocketFd) when the thread has a tag or a uid.
        }

        @Override
        public void untag(FileDescriptor fd) throws SocketException {
            if (LOGD) {
                Log.i(TAG, "untagSocket(" + fd.getInt$() + ")");
            }
            // Android's native_untagSocketFd, left out as the tag is.
        }
    }

    private static class UidTag {
        public int tag = -1;
        public int uid = -1;
    }

    private static ThreadLocal<UidTag> sThreadUidTag = new ThreadLocal<UidTag>() {
        @Override
        protected UidTag initialValue() {
            return new UidTag();
        }
    };

    public static void setThreadStatsTag(int tag) {
        getAndSetThreadStatsTag(tag);
    }

    public static int getAndSetThreadStatsTag(int tag) {
        final int old = sThreadUidTag.get().tag;
        sThreadUidTag.get().tag = tag;
        return old;
    }

    /** @hide */
    public static void setThreadStatsTagBackup() {
        setThreadStatsTag(TAG_SYSTEM_BACKUP);
    }

    /** @hide */
    public static void setThreadStatsTagRestore() {
        setThreadStatsTag(TAG_SYSTEM_RESTORE);
    }

    /** @hide */
    public static void setThreadStatsTagApp() {
        setThreadStatsTag(TAG_SYSTEM_APP);
    }

    /** @hide */
    public static void setThreadStatsTagDownload() {
        setThreadStatsTag(TAG_SYSTEM_DOWNLOAD);
    }

    public static int getThreadStatsTag() {
        return sThreadUidTag.get().tag;
    }

    public static void clearThreadStatsTag() {
        sThreadUidTag.get().tag = -1;
    }

    public static void setThreadStatsUid(int uid) {
        sThreadUidTag.get().uid = uid;
    }

    public static int getThreadStatsUid() {
        return sThreadUidTag.get().uid;
    }

    /** @hide */
    @Deprecated
    public static void setThreadStatsUidSelf() {
        setThreadStatsUid(android.os.Process.myUid());
    }

    public static void clearThreadStatsUid() {
        setThreadStatsUid(-1);
    }

    public static void tagSocket(Socket socket) throws SocketException {
        dalvik.system.SocketTagger.get().tag(socket);
    }

    public static void untagSocket(Socket socket) throws SocketException {
        dalvik.system.SocketTagger.get().untag(socket);
    }

    public static void tagDatagramSocket(DatagramSocket socket) throws SocketException {
        dalvik.system.SocketTagger.get().tag(socket);
    }

    public static void untagDatagramSocket(DatagramSocket socket) throws SocketException {
        dalvik.system.SocketTagger.get().untag(socket);
    }

    public static void tagFileDescriptor(FileDescriptor fd) throws IOException {
        dalvik.system.SocketTagger.get().tag(fd);
    }

    public static void untagFileDescriptor(FileDescriptor fd) throws IOException {
        dalvik.system.SocketTagger.get().untag(fd);
    }

    public static void incrementOperationCount(int operationCount) {
        final int tag = getThreadStatsTag();
        incrementOperationCount(tag, operationCount);
    }

    // NetworkStatsService's checks for the app's own uid; it adds the count to its statistics, which Minato has none
    // of.
    public static void incrementOperationCount(int tag, int operationCount) {
        if (operationCount < 0) {
            throw new IllegalArgumentException("operation count can only be incremented");
        }
        if (tag == 0) {  // NetworkStats.TAG_NONE
            throw new IllegalArgumentException("operation count must have specific tag");
        }
    }

    private static long addIfSupported(long stat) {
        return (stat == UNSUPPORTED) ? 0 : stat;
    }

    public static long getMobileTxPackets() {
        long total = 0;
        for (String iface : getMobileIfaces()) {
            total += addIfSupported(getTxPackets(iface));
        }
        return total;
    }

    public static long getMobileRxPackets() {
        long total = 0;
        for (String iface : getMobileIfaces()) {
            total += addIfSupported(getRxPackets(iface));
        }
        return total;
    }

    public static long getMobileTxBytes() {
        long total = 0;
        for (String iface : getMobileIfaces()) {
            total += addIfSupported(getTxBytes(iface));
        }
        return total;
    }

    public static long getMobileRxBytes() {
        long total = 0;
        for (String iface : getMobileIfaces()) {
            total += addIfSupported(getRxBytes(iface));
        }
        return total;
    }

    /** @hide */
    @Deprecated
    public static long getMobileTcpRxPackets() {
        return UNSUPPORTED;
    }

    /** @hide */
    @Deprecated
    public static long getMobileTcpTxPackets() {
        return UNSUPPORTED;
    }

    public static long getTxPackets(String iface) {
        return getIfaceStats(iface, TYPE_TX_PACKETS);
    }

    public static long getRxPackets(String iface) {
        return getIfaceStats(iface, TYPE_RX_PACKETS);
    }

    public static long getTxBytes(String iface) {
        return getIfaceStats(iface, TYPE_TX_BYTES);
    }

    public static long getRxBytes(String iface) {
        return getIfaceStats(iface, TYPE_RX_BYTES);
    }

    /** @hide */
    public static long getLoopbackTxPackets() {
        return getIfaceStats("lo", TYPE_TX_PACKETS);
    }

    /** @hide */
    public static long getLoopbackRxPackets() {
        return getIfaceStats("lo", TYPE_RX_PACKETS);
    }

    /** @hide */
    public static long getLoopbackTxBytes() {
        return getIfaceStats("lo", TYPE_TX_BYTES);
    }

    /** @hide */
    public static long getLoopbackRxBytes() {
        return getIfaceStats("lo", TYPE_RX_BYTES);
    }

    public static long getTotalTxPackets() {
        return getTotalStats(TYPE_TX_PACKETS);
    }

    public static long getTotalRxPackets() {
        return getTotalStats(TYPE_RX_PACKETS);
    }

    public static long getTotalTxBytes() {
        return getTotalStats(TYPE_TX_BYTES);
    }

    public static long getTotalRxBytes() {
        return getTotalStats(TYPE_RX_BYTES);
    }

    public static long getUidTxBytes(int uid) {
        return getUidStats(uid, TYPE_TX_BYTES);
    }

    public static long getUidRxBytes(int uid) {
        return getUidStats(uid, TYPE_RX_BYTES);
    }

    public static long getUidTxPackets(int uid) {
        return getUidStats(uid, TYPE_TX_PACKETS);
    }

    public static long getUidRxPackets(int uid) {
        return getUidStats(uid, TYPE_RX_PACKETS);
    }

    // The transport layer's counters, deprecated: UNSUPPORTED since Android N.
    @Deprecated
    public static long getUidTcpTxBytes(int uid) {
        return UNSUPPORTED;
    }

    @Deprecated
    public static long getUidTcpRxBytes(int uid) {
        return UNSUPPORTED;
    }

    @Deprecated
    public static long getUidUdpTxBytes(int uid) {
        return UNSUPPORTED;
    }

    @Deprecated
    public static long getUidUdpRxBytes(int uid) {
        return UNSUPPORTED;
    }

    @Deprecated
    public static long getUidTcpTxSegments(int uid) {
        return UNSUPPORTED;
    }

    @Deprecated
    public static long getUidTcpRxSegments(int uid) {
        return UNSUPPORTED;
    }

    @Deprecated
    public static long getUidUdpTxPackets(int uid) {
        return UNSUPPORTED;
    }

    @Deprecated
    public static long getUidUdpRxPackets(int uid) {
        return UNSUPPORTED;
    }

    // NetworkStatsService's answers. Another uid's counters are UNSUPPORTED for an app, as on Android since N; the
    // app's own are UNSUPPORTED too, the named difference above.
    private static long getUidStats(int uid, int type) {
        return UNSUPPORTED;
    }

    // An interface's counters: UNSUPPORTED, the named difference above; Android 15 counts every interface and gives
    // even a name it never saw 0. A null name throws first, as NetworkStatsService's check does.
    private static long getIfaceStats(String iface, int type) {
        Objects.requireNonNull(iface);
        return UNSUPPORTED;
    }

    // The device's totals: UNSUPPORTED, the named difference above.
    private static long getTotalStats(int type) {
        return UNSUPPORTED;
    }

    // NetworkStatsService's mobile interfaces, those of cellular networks: none, as Minato reports no network.
    private static String[] getMobileIfaces() {
        return new String[0];
    }
}
