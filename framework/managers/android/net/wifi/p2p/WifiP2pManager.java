// The written part of WifiP2pManager (services bundle C, part 2 of the system services gap): Wi-Fi Direct. Minato has
// no Wi-Fi Direct (iOS has none for apps), so the manager hands out its channel and every action fails with ERROR (0)
// on the channel's looper, as the emulator (apk-runner-35-bare) answers discoverPeers [verified: services-i-test];
// the other action calls are the same answer by AOSP's code path [inferred]. The request* calls give no answer.
import android.content.Context;
import android.net.wifi.p2p.nsd.WifiP2pServiceInfo;
import android.net.wifi.p2p.nsd.WifiP2pServiceRequest;
import android.os.Handler;
import android.os.Looper;

class WifiP2pManager {
    /** @hide As the tag's, which SystemServiceRegistry calls with the outer context. */
    public WifiP2pManager() {
    }

    public Channel initialize(Context srcContext, Looper srcLooper, ChannelListener listener) {
        return new Channel(srcLooper != null ? srcLooper : Looper.getMainLooper());
    }

    private static void fail(Channel channel, ActionListener listener) {
        if (channel == null) throw new IllegalArgumentException("Channel needs to be initialized");
        if (listener != null) channel.mHandler.post(() -> listener.onFailure(ERROR));
    }

    public void discoverPeers(Channel c, ActionListener listener) {
        fail(c, listener);
    }

    public void stopPeerDiscovery(Channel c, ActionListener listener) {
        fail(c, listener);
    }

    public void connect(Channel c, WifiP2pConfig config, ActionListener listener) {
        fail(c, listener);
    }

    public void cancelConnect(Channel c, ActionListener listener) {
        fail(c, listener);
    }

    public void createGroup(Channel c, ActionListener listener) {
        fail(c, listener);
    }

    public void createGroup(Channel c, WifiP2pConfig config, ActionListener listener) {
        fail(c, listener);
    }

    public void removeGroup(Channel c, ActionListener listener) {
        fail(c, listener);
    }

    public void addLocalService(Channel c, WifiP2pServiceInfo servInfo, ActionListener listener) {
        fail(c, listener);
    }

    public void removeLocalService(Channel c, WifiP2pServiceInfo servInfo, ActionListener listener) {
        fail(c, listener);
    }

    public void clearLocalServices(Channel c, ActionListener listener) {
        fail(c, listener);
    }

    public void addServiceRequest(Channel c, WifiP2pServiceRequest req, ActionListener listener) {
        fail(c, listener);
    }

    public void removeServiceRequest(Channel c, WifiP2pServiceRequest req, ActionListener listener) {
        fail(c, listener);
    }

    public void clearServiceRequests(Channel c, ActionListener listener) {
        fail(c, listener);
    }

    public void discoverServices(Channel c, ActionListener listener) {
        fail(c, listener);
    }

    public static class Channel implements AutoCloseable {
        private final Handler mHandler;

        private Channel(Looper looper) {
            mHandler = new Handler(looper);
        }

        public void close() {
        }
    }
}
