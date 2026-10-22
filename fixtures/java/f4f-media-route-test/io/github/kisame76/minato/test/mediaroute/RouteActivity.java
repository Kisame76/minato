package io.github.kisame76.minato.test.mediaroute;

import android.app.Activity;
import android.content.ComponentName;
import android.media.EncoderProfiles;
import android.media.MediaRoute2Info;
import android.media.RouteDiscoveryPreference;
import android.media.RouteListingPreference;
import android.media.RoutingSessionInfo;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.Parcel;
import android.util.Log;
import java.lang.reflect.Modifier;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashSet;
import java.util.List;
import java.util.TreeSet;

/**
 * The test of framework breadth batch F4f: android.media's routing data as a media app builds and reads it, routes,
 * routing sessions and the discovery and listing preferences, and the encoder profiles, which only the recorder
 * makes and which must load. It runs on the main thread when the window first has the focus and logs under the tag
 * f4f-media-route-test; tools/android-reference.sh takes the log from Android 15, and
 * tools/f4f-media-route-check.sh compares Minato's with it.
 */
public class RouteActivity extends Activity {
    static final String TAG = "f4f-media-route-test";
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private boolean mStarted;

    private static void log(String line) {
        Log.i(TAG, line);
    }

    private interface Part {
        void run() throws Exception;
    }

    private static void part(String name, Part part) {
        try {
            part.run();
        } catch (Throwable t) {
            StringBuilder line = new StringBuilder(name);
            for (Throwable c = t; c != null; c = c.getCause()) {
                line.append(": ").append(c.getClass().getName());
            }
            log(line.toString());
        }
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (!hasFocus || mStarted) {
            return;
        }
        mStarted = true;
        mHandler.post(() -> {
            part("route", this::route);
            part("session", this::session);
            part("discovery", this::discovery);
            part("listing", this::listing);
            part("encoder", this::encoder);
            log("done");
            finish();
        });
    }

    private static <T extends android.os.Parcelable> T through(T value, android.os.Parcelable.Creator<T> creator) {
        Parcel parcel = Parcel.obtain();
        value.writeToParcel(parcel, 0);
        parcel.setDataPosition(0);
        T back = creator.createFromParcel(parcel);
        parcel.recycle();
        return back;
    }

    private void route() {
        Bundle extras = new Bundle();
        extras.putString("key", "value");
        MediaRoute2Info route = new MediaRoute2Info.Builder("route1", "Living room")
                .setDescription("A speaker")
                .addFeature(MediaRoute2Info.FEATURE_REMOTE_PLAYBACK)
                .addFeature(MediaRoute2Info.FEATURE_LIVE_AUDIO)
                .setType(MediaRoute2Info.TYPE_REMOTE_SPEAKER)
                .setConnectionState(MediaRoute2Info.CONNECTION_STATE_CONNECTED)
                .setVolume(5)
                .setVolumeMax(10)
                .setVolumeHandling(MediaRoute2Info.PLAYBACK_VOLUME_VARIABLE)
                .setIconUri(Uri.parse("content://icons/speaker"))
                .setExtras(extras)
                .setDeduplicationIds(new HashSet<>(Arrays.asList("b", "a")))
                .setClientPackageName("com.example.player")
                .build();
        log("route: id=" + route.getId() + " name=" + route.getName() + " description=" + route.getDescription()
                + " features=" + route.getFeatures() + " type=" + route.getType() + " connection="
                + route.getConnectionState() + " volume=" + route.getVolume() + "/" + route.getVolumeMax()
                + " handling=" + route.getVolumeHandling() + " icon=" + route.getIconUri() + " extras="
                + route.getExtras().getString("key") + " dedup=" + new TreeSet<>(route.getDeduplicationIds())
                + " client=" + route.getClientPackageName() + " system=" + route.isSystemRoute());
        MediaRoute2Info back = through(route, MediaRoute2Info.CREATOR);
        log("route parcel: equal=" + back.equals(route) + " name=" + back.getName() + " features="
                + back.getFeatures() + " describe=" + back.describeContents());
        part("route copy", () -> {
            MediaRoute2Info copy = new MediaRoute2Info.Builder(route).setVolume(7).clearFeatures().build();
            log("route copy: equal=" + copy.equals(route) + " volume=" + copy.getVolume() + " features="
                    + copy.getFeatures() + " id=" + copy.getId());
        });
        try {
            new MediaRoute2Info.Builder("", "name");
        } catch (IllegalArgumentException e) {
            log("route empty id: IllegalArgumentException");
        }
        try {
            new MediaRoute2Info.Builder("id", "");
        } catch (IllegalArgumentException e) {
            log("route empty name: IllegalArgumentException");
        }
        log("route constants: " + MediaRoute2Info.TYPE_UNKNOWN + " " + MediaRoute2Info.TYPE_BUILTIN_SPEAKER + " "
                + MediaRoute2Info.TYPE_BLUETOOTH_A2DP + " " + MediaRoute2Info.TYPE_REMOTE_TV + " "
                + MediaRoute2Info.TYPE_GROUP + " " + MediaRoute2Info.PLAYBACK_VOLUME_FIXED + " "
                + MediaRoute2Info.FEATURE_REMOTE_VIDEO_PLAYBACK);
    }

    private void session() {
        Bundle hints = new Bundle();
        hints.putInt("hint", 3);
        RoutingSessionInfo session = new RoutingSessionInfo.Builder("session1", "com.example.player")
                .setName("Living room session")
                .addSelectedRoute("route1")
                .addSelectableRoute("route2")
                .addDeselectableRoute("route1")
                .addTransferableRoute("route3")
                .setVolume(4)
                .setVolumeMax(8)
                .setVolumeHandling(MediaRoute2Info.PLAYBACK_VOLUME_VARIABLE)
                .setControlHints(hints)
                .build();
        log("session: id=" + session.getId() + " client=" + session.getClientPackageName() + " name="
                + session.getName() + " selected=" + session.getSelectedRoutes() + " selectable="
                + session.getSelectableRoutes() + " deselectable=" + session.getDeselectableRoutes()
                + " transferable=" + session.getTransferableRoutes() + " volume=" + session.getVolume() + "/"
                + session.getVolumeMax() + " handling=" + session.getVolumeHandling() + " hints="
                + session.getControlHints().getInt("hint"));
        RoutingSessionInfo back = through(session, RoutingSessionInfo.CREATOR);
        log("session parcel: equal=" + back.equals(session) + " selected=" + back.getSelectedRoutes());
        part("session copy", () -> {
            RoutingSessionInfo copy = new RoutingSessionInfo.Builder(session).clearSelectableRoutes()
                    .removeSelectedRoute("route1").build();
            log("session copy: selected=" + copy.getSelectedRoutes() + " selectable=" + copy.getSelectableRoutes());
        });
        try {
            new RoutingSessionInfo.Builder("", "pkg");
        } catch (IllegalArgumentException e) {
            log("session empty id: IllegalArgumentException");
        }
    }

    private void discovery() {
        RouteDiscoveryPreference preference = new RouteDiscoveryPreference.Builder(
                Arrays.asList(MediaRoute2Info.FEATURE_REMOTE_PLAYBACK, MediaRoute2Info.FEATURE_LIVE_VIDEO), true)
                .build();
        log("discovery: features=" + preference.getPreferredFeatures() + " activeScan="
                + preference.shouldPerformActiveScan());
        RouteDiscoveryPreference back = through(preference, RouteDiscoveryPreference.CREATOR);
        RouteDiscoveryPreference changed = new RouteDiscoveryPreference.Builder(preference)
                .setShouldPerformActiveScan(false).build();
        log("discovery parcel: equal=" + back.equals(preference) + " changed=" + changed.shouldPerformActiveScan()
                + " equalChanged=" + changed.equals(preference));
    }

    private void listing() {
        List<RouteListingPreference.Item> items = new ArrayList<>();
        items.add(new RouteListingPreference.Item.Builder("route1")
                .setFlags(RouteListingPreference.Item.FLAG_SUGGESTED)
                .setSubText(RouteListingPreference.Item.SUBTEXT_SUBSCRIPTION_REQUIRED)
                .setSelectionBehavior(RouteListingPreference.Item.SELECTION_BEHAVIOR_GO_TO_APP)
                .build());
        items.add(new RouteListingPreference.Item.Builder("route2")
                .setSubText(RouteListingPreference.Item.SUBTEXT_CUSTOM)
                .setCustomSubtextMessage("Custom message")
                .build());
        RouteListingPreference preference = new RouteListingPreference.Builder()
                .setItems(items)
                .setUseSystemOrdering(false)
                .setLinkedItemComponentName(new ComponentName("com.example.player", "com.example.player.Linked"))
                .build();
        for (RouteListingPreference.Item item : preference.getItems()) {
            log("listing item: route=" + item.getRouteId() + " flags=" + item.getFlags() + " subText="
                    + item.getSubText() + " custom=" + item.getCustomSubtextMessage() + " behavior="
                    + item.getSelectionBehavior());
        }
        log("listing: items=" + preference.getItems().size() + " systemOrdering=" + preference.getUseSystemOrdering()
                + " linked=" + preference.getLinkedItemComponentName());
        RouteListingPreference back = through(preference, RouteListingPreference.CREATOR);
        log("listing parcel: equal=" + back.equals(preference) + " items=" + back.getItems().size());
        try {
            new RouteListingPreference.Item.Builder("route3").setSubText(RouteListingPreference.Item.SUBTEXT_CUSTOM)
                    .build();
        } catch (IllegalArgumentException e) {
            log("listing custom without message: IllegalArgumentException");
        }
        try {
            new RouteListingPreference.Item.Builder("");
        } catch (IllegalArgumentException e) {
            log("listing empty route: IllegalArgumentException");
        }
        log("listing constants: " + RouteListingPreference.ACTION_TRANSFER_MEDIA + " "
                + RouteListingPreference.EXTRA_ROUTE_ID + " " + RouteListingPreference.Item.FLAG_ONGOING_SESSION
                + " " + RouteListingPreference.Item.SELECTION_BEHAVIOR_TRANSFER + " "
                + RouteListingPreference.Item.SUBTEXT_TRACK_UNSUPPORTED);
    }

    /** The encoder profiles only the recorder makes: they load, final or not as AOSP's are. */
    private void encoder() throws Exception {
        for (Class<?> c : new Class<?>[] {EncoderProfiles.class, EncoderProfiles.VideoProfile.class,
            EncoderProfiles.AudioProfile.class}) {
            log("encoder: " + c.getSimpleName() + " final=" + Modifier.isFinal(c.getModifiers()) + " public="
                    + Modifier.isPublic(c.getModifiers()));
        }
        log("encoder constants: " + EncoderProfiles.VideoProfile.HDR_NONE + " "
                + EncoderProfiles.VideoProfile.HDR_HLG + " " + EncoderProfiles.VideoProfile.HDR_DOLBY_VISION + " "
                + EncoderProfiles.VideoProfile.YUV_420 + " " + EncoderProfiles.VideoProfile.YUV_444);
    }
}
