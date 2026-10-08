package io.github.kisame76.minato.test.traffic;

import android.app.Application;
import android.net.TrafficStats;
import android.os.Process;
import android.os.StrictMode;
import android.os.strictmode.Violation;
import android.system.ErrnoException;
import android.system.Os;
import android.system.OsConstants;
import android.util.Log;

import java.io.FileDescriptor;
import java.io.IOException;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/**
 * TrafficStats as an app sees it: the thread's statistics tag and uid, the socket tagger that the app process attaches
 * at its start (StrictMode's untagged socket violation shows it), tagging sockets and descriptors, the counters that
 * Android answers UNSUPPORTED for, and incrementOperationCount's checks. What the device counts (the app's own bytes,
 * the totals, an interface's, the mobile and loopback interfaces) goes on the device tag: a device that counts
 * nothing answers UNSUPPORTED there, where Android 15 gives even an interface it never saw 0.
 */
public class TrafficApplication extends Application {
    static final String TAG = "traffic-test";

    private static void log(String line) {
        Log.i(TAG, line);
    }

    private static void device(String line) {
        Log.i(TAG + "-device", line);
    }

    private static String hex(int tag) {
        return "0x" + Integer.toHexString(tag);
    }

    @Override
    public void onCreate() {
        super.onCreate();
        threadTags();
        tagger();
        tagging();
        counters();
        operationCounts();
        log("done");
    }

    private void threadTags() {
        log("thread tag " + hex(TrafficStats.getThreadStatsTag()) + ", uid " + TrafficStats.getThreadStatsUid());
        TrafficStats.setThreadStatsTag(0x1234);
        log("tag set: " + hex(TrafficStats.getThreadStatsTag()));
        int old = TrafficStats.getAndSetThreadStatsTag(0x5678);
        log("getAndSet: old " + hex(old) + ", now " + hex(TrafficStats.getThreadStatsTag()));
        TrafficStats.clearThreadStatsTag();
        log("tag cleared: " + hex(TrafficStats.getThreadStatsTag()));
        TrafficStats.setThreadStatsUid(Process.myUid());
        log("uid set to the app's own: " + (TrafficStats.getThreadStatsUid() == Process.myUid()));
        TrafficStats.clearThreadStatsUid();
        log("uid cleared: " + TrafficStats.getThreadStatsUid());

        // Each thread has its own tag and uid, from -1.
        TrafficStats.setThreadStatsTag(0x42);
        TrafficStats.setThreadStatsUid(Process.myUid());
        final int[] other = new int[2];
        Thread thread = new Thread(() -> {
            other[0] = TrafficStats.getThreadStatsTag();
            other[1] = TrafficStats.getThreadStatsUid();
        });
        thread.start();
        try {
            thread.join();
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
        log("another thread: tag " + hex(other[0]) + ", uid " + other[1] + "; this thread's tag still "
                + hex(TrafficStats.getThreadStatsTag()));
        TrafficStats.clearThreadStatsTag();
        TrafficStats.clearThreadStatsUid();
    }

    // The tagger that RuntimeInit attaches tags every new socket and tells StrictMode of one without a tag.
    private void tagger() {
        StrictMode.VmPolicy before = StrictMode.getVmPolicy();
        List<Violation> seen = Collections.synchronizedList(new ArrayList<>());
        StrictMode.setVmPolicy(new StrictMode.VmPolicy.Builder().detectUntaggedSockets()
                .penaltyListener(Runnable::run, seen::add).build());
        try {
            try (DatagramSocket socket = new DatagramSocket()) {
                log("an untagged socket: StrictMode saw " + describe(seen));
            }
            seen.clear();
            TrafficStats.setThreadStatsTag(0x42);
            try (DatagramSocket socket = new DatagramSocket()) {
                log("a socket with tag " + hex(TrafficStats.getThreadStatsTag()) + ": StrictMode saw "
                        + describe(seen));
            }
        } catch (IOException e) {
            log("the tagger: " + e);
        } finally {
            TrafficStats.clearThreadStatsTag();
            StrictMode.setVmPolicy(before);
        }
    }

    private static String describe(List<Violation> seen) {
        if (seen.isEmpty()) {
            return "nothing";
        }
        Violation first = seen.get(0);
        return seen.size() + ", " + first.getClass().getName() + ": " + first.getMessage();
    }

    private void tagging() {
        TrafficStats.setThreadStatsTag(0x42);
        try (Socket socket = new Socket()) {
            socket.bind(new InetSocketAddress(InetAddress.getLoopbackAddress(), 0));
            TrafficStats.tagSocket(socket);
            TrafficStats.untagSocket(socket);
            log("tag and untag a bound socket: fine");
        } catch (IOException e) {
            log("tag and untag a bound socket: " + e);
        }
        try (DatagramSocket socket = new DatagramSocket()) {
            TrafficStats.tagDatagramSocket(socket);
            TrafficStats.untagDatagramSocket(socket);
            log("tag and untag a datagram socket: fine");
        } catch (IOException e) {
            log("tag and untag a datagram socket: " + e);
        }
        try {
            FileDescriptor fd = Os.socket(OsConstants.AF_INET, OsConstants.SOCK_STREAM, 0);
            try {
                TrafficStats.tagFileDescriptor(fd);
                TrafficStats.untagFileDescriptor(fd);
                log("tag and untag a socket's descriptor: fine");
            } finally {
                Os.close(fd);
            }
        } catch (ErrnoException | IOException e) {
            log("tag and untag a socket's descriptor: " + e);
        }
        TrafficStats.clearThreadStatsTag();
    }

    @SuppressWarnings("deprecation")
    private void counters() {
        int own = Process.myUid();
        log("the transport layer's counters of the app's own uid: " + TrafficStats.getUidTcpTxBytes(own) + " "
                + TrafficStats.getUidTcpRxBytes(own) + " " + TrafficStats.getUidUdpTxBytes(own) + " "
                + TrafficStats.getUidUdpRxBytes(own) + " " + TrafficStats.getUidTcpTxSegments(own) + " "
                + TrafficStats.getUidTcpRxSegments(own) + " " + TrafficStats.getUidUdpTxPackets(own) + " "
                + TrafficStats.getUidUdpRxPackets(own));
        int system = Process.SYSTEM_UID;
        log("the system uid's counters: " + TrafficStats.getUidTxBytes(system) + " "
                + TrafficStats.getUidRxBytes(system) + " " + TrafficStats.getUidTxPackets(system) + " "
                + TrafficStats.getUidRxPackets(system));
        device("an interface that is not there: " + TrafficStats.getTxBytes("minato0") + " "
                + TrafficStats.getRxBytes("minato0") + " " + TrafficStats.getTxPackets("minato0") + " "
                + TrafficStats.getRxPackets("minato0"));
        try {
            log("no interface: " + TrafficStats.getRxBytes(null));
        } catch (RuntimeException e) {
            log("no interface: " + e.getClass().getName() + ": " + e.getMessage());
        }
        device("the app's own uid: " + TrafficStats.getUidTxBytes(own) + " " + TrafficStats.getUidRxBytes(own) + " "
                + TrafficStats.getUidTxPackets(own) + " " + TrafficStats.getUidRxPackets(own));
        device("totals: " + TrafficStats.getTotalTxBytes() + " " + TrafficStats.getTotalRxBytes() + " "
                + TrafficStats.getTotalTxPackets() + " " + TrafficStats.getTotalRxPackets());
        device("mobile: " + TrafficStats.getMobileTxBytes() + " " + TrafficStats.getMobileRxBytes() + " "
                + TrafficStats.getMobileTxPackets() + " " + TrafficStats.getMobileRxPackets());
        device("loopback: " + TrafficStats.getTxBytes("lo") + " " + TrafficStats.getRxBytes("lo") + " "
                + TrafficStats.getTxPackets("lo") + " " + TrafficStats.getRxPackets("lo"));
    }

    private void operationCounts() {
        count("the thread's tag " + hex(TrafficStats.getThreadStatsTag()) + ", 1", () ->
                TrafficStats.incrementOperationCount(1));
        count("tag 0x42, 3", () -> TrafficStats.incrementOperationCount(0x42, 3));
        count("tag 0, 1", () -> TrafficStats.incrementOperationCount(0, 1));
        count("tag 0x42, -1", () -> TrafficStats.incrementOperationCount(0x42, -1));
    }

    private static void count(String what, Runnable call) {
        try {
            call.run();
            log("incrementOperationCount(" + what + "): fine");
        } catch (RuntimeException e) {
            log("incrementOperationCount(" + what + "): " + e.getClass().getName() + ": " + e.getMessage());
        }
    }
}
