package android.app;

import android.content.Intent;

/**
 * @hide The network's broadcasts (ADR-0054 step d)): LocalNetwork's CONNECTIVITY_ACTION and
 * WIFI_STATE_CHANGED_ACTION, sent sticky as system_server's ConnectivityService and WifiService send them, and
 * ACTION_RESTRICT_BACKGROUND_CHANGED, as NetworkPolicyManagerService sends it, through the local activity manager
 * service.
 */
public final class LocalNetworkBroadcasts {
    private LocalNetworkBroadcasts() {}

    public static void sendSticky(Intent intent) {
        LocalActivityManagerService.instance().broadcastStickyFromSystem(intent);
    }

    public static void send(Intent intent) {
        LocalActivityManagerService.instance().broadcastFromSystem(intent);
    }
}
