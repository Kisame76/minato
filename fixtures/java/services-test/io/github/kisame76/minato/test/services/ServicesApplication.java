package io.github.kisame76.minato.test.services;

import android.app.ActivityManager;
import android.app.AppOpsManager;
import android.app.Application;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationChannelGroup;
import android.app.NotificationManager;
import android.content.Context;
import android.content.pm.PackageManager;
import android.hardware.display.DisplayManager;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;
import android.media.AudioManager;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.wifi.WifiManager;
import android.os.Environment;
import android.os.PowerManager;
import android.os.Process;
import android.os.UserHandle;
import android.os.UserManager;
import android.os.storage.StorageManager;
import android.os.storage.StorageVolume;
import android.speech.tts.UtteranceProgressListener;
import android.util.Log;
import android.view.Display;

import java.util.ArrayList;
import java.util.List;
import java.util.TreeSet;

/**
 * ADR-0045's test of the local services that Godot's start asks for, in
 * Application.onCreate, before its launcher activity starts:
 * <ul>
 *   <li>a subclass of UtteranceProgressListener, whose default onError(id, code) calls the older onError(id)
 *       and whose other defaults do nothing;
 *   <li>the sensor service on a device without sensors: its class, the list, the default sensors and a listener
 *       without a sensor;
 *   <li>the Wi-Fi feature, and the Wi-Fi service's multicast lock: held and released by its reference count,
 *       under-locked, without a reference count, and refused past 50 locks; a Wi-Fi lock without WAKE_LOCK, refused
 *       at its acquire and at its release;
 *   <li>whether Wi-Fi is on, which the service refuses without ACCESS_WIFI_STATE, as the app does not request it;
 *   <li>the storage service (stop 4): its one volume, the primary emulated one on both, found again from the app's
 *       external files, and whether the app has legacy storage or manages all files;
 *   <li>the app ops service (stop 6): the modes of a runtime permission's op that the app requests and is denied
 *       (CAMERA), of one it does not request (RECORD_AUDIO), of all-files access, picture in picture and no isolated
 *       storage; a note; a check that throws for an errored op; an unknown op.
 * </ul>
 * Part 2 (the services that answered null): the activity, display, power, audio and
 * connectivity services, each with its class, the same object by its class, and its answers; a wake lock without
 * WAKE_LOCK, refused. apk-runner-35-bare has a network after all (its emulated modem's), so the network's state is
 * shown on the device tag.
 * Part 3: the user service, with its class, the same object by its class, and Android's one user's
 * answers: unlocked, the system user, no profile, no restriction on installing apps, running; the calls that need a
 * system permission, refused. The user's restrictions and multi-user support, which the device's build sets, go to
 * the device tag.
 * Part 4 (the notification service): its class and answers for an app without POST_NOTIFICATIONS,
 * which Android 15 does not let post; channel groups and channels, each kept as a copy, created again with a new
 * name, a higher and a lower importance, refused for a group that does not exist, the reserved ID and an importance
 * out of range, deleted and created again; a notification built for a channel, posted and gone, one without a small
 * icon refused before it is posted, and the cancels.
 * apk-runner-35-bare has no sensors, as Minato has none, so every line is the same on both; an exception's message,
 * which names the app's uid, is left out. What differs by device (memory, the display's name and rate, the audio
 * output's rate and buffer, the network) goes to the device tag, which is shown, not compared.
 */
public class ServicesApplication extends Application {
    private static final String TAG = "services-test";
    private static final String DEVICE = TAG + "-device";

    private interface Section {
        void run() throws Exception;
    }

    @Override
    public void onCreate() {
        super.onCreate();
        guarded("UtteranceProgressListener", this::speechListener);
        guarded("sensor service", this::sensors);
        guarded("wifi service", this::wifiLocks);
        guarded("storage service", this::storage);
        guarded("app ops service", this::appOps);
        guarded("activity service", this::activity);
        guarded("display service", this::display);
        guarded("power service", this::power);
        guarded("audio service", this::audio);
        guarded("connectivity service", this::connectivity);
        guarded("user service", this::user);
        guarded("notification service", this::notifications);
        Log.i(TAG, "done");
    }

    private void speechListener() {
        final StringBuilder calls = new StringBuilder();
        UtteranceProgressListener listener = new UtteranceProgressListener() {
            @Override
            public void onStart(String utteranceId) {
                calls.append(" onStart(").append(utteranceId).append(')');
            }

            @Override
            public void onDone(String utteranceId) {
                calls.append(" onDone(").append(utteranceId).append(')');
            }

            @Override
            public void onError(String utteranceId) {
                calls.append(" onError(").append(utteranceId).append(')');
            }
        };
        // TextToSpeech.ERROR_NETWORK, written out so that TextToSpeech, which Minato does not have yet, is not named.
        listener.onError("u", -6);
        listener.onRangeStart("u", 1, 2, 3);
        listener.onStop("u", true);
        listener.onBeginSynthesis("u", 16000, 2, 1);
        listener.onAudioAvailable("u", new byte[0]);
        Log.i(TAG, "UtteranceProgressListener's defaults called:" + calls);
    }

    private void sensors() {
        SensorManager sensors = (SensorManager) getSystemService(Context.SENSOR_SERVICE);
        Log.i(TAG, "sensor service: " + sensors.getClass().getName());
        Log.i(TAG, "sensors: " + sensors.getSensorList(Sensor.TYPE_ALL).size() + ", dynamic discovery "
                + sensors.isDynamicSensorDiscoverySupported());
        Log.i(TAG, "default sensors: accelerometer " + sensors.getDefaultSensor(Sensor.TYPE_ACCELEROMETER)
                + ", gyroscope " + sensors.getDefaultSensor(Sensor.TYPE_GYROSCOPE) + ", magnetic field "
                + sensors.getDefaultSensor(Sensor.TYPE_MAGNETIC_FIELD) + ", gravity "
                + sensors.getDefaultSensor(Sensor.TYPE_GRAVITY));
        SensorEventListener listener = new SensorEventListener() {
            @Override
            public void onSensorChanged(SensorEvent event) {
            }

            @Override
            public void onAccuracyChanged(Sensor sensor, int accuracy) {
            }
        };
        Log.i(TAG, "registerListener without a sensor: "
                + sensors.registerListener(listener, null, SensorManager.SENSOR_DELAY_GAME));
    }

    private void wifiLocks() {
        Log.i(TAG, "Wi-Fi feature: " + getPackageManager().hasSystemFeature(PackageManager.FEATURE_WIFI));
        WifiManager wifi = (WifiManager) getSystemService(Context.WIFI_SERVICE);
        Log.i(TAG, "wifi service: " + wifi.getClass().getName());
        try {
            Log.i(TAG, "isWifiEnabled without ACCESS_WIFI_STATE: " + wifi.isWifiEnabled());
        } catch (SecurityException e) {
            Log.i(TAG, "isWifiEnabled without ACCESS_WIFI_STATE: " + e.getClass().getName());
        }
        WifiManager.MulticastLock lock = wifi.createMulticastLock(TAG);
        StringBuilder held = new StringBuilder("multicast lock held: " + lock.isHeld());
        lock.acquire();
        held.append(", after acquire ").append(lock.isHeld());
        lock.acquire();
        lock.release();
        held.append(", after a second acquire and one release ").append(lock.isHeld());
        lock.release();
        held.append(", after the second release ").append(lock.isHeld());
        Log.i(TAG, held.toString());
        guarded("one release more", () -> lock.release());

        WifiManager.MulticastLock plain = wifi.createMulticastLock(TAG + "-plain");
        plain.setReferenceCounted(false);
        plain.acquire();
        plain.acquire();
        plain.release();
        Log.i(TAG, "without a reference count, acquired twice and released once: held " + plain.isHeld());

        List<WifiManager.MulticastLock> locks = new ArrayList<>();
        try {
            for (int i = 0; i < 51; i++) {
                WifiManager.MulticastLock next = wifi.createMulticastLock(TAG + "-" + i);
                next.acquire();
                locks.add(next);
            }
            Log.i(TAG, "51 multicast locks held");
        } catch (RuntimeException e) {
            Log.i(TAG, "multicast lock " + (locks.size() + 1) + ": " + e);
        }
        for (WifiManager.MulticastLock each : locks) {
            each.release();
        }

        WifiManager.WifiLock wifiLock = wifi.createWifiLock(WifiManager.WIFI_MODE_FULL_HIGH_PERF, TAG);
        try {
            wifiLock.acquire();
            Log.i(TAG, "Wi-Fi lock without WAKE_LOCK: held " + wifiLock.isHeld());
        } catch (SecurityException e) {
            Log.i(TAG, "Wi-Fi lock without WAKE_LOCK: " + e.getClass().getName() + ", held " + wifiLock.isHeld());
        }
        try {
            wifiLock.release();
            Log.i(TAG, "Wi-Fi lock release without WAKE_LOCK: held " + wifiLock.isHeld());
        } catch (SecurityException e) {
            Log.i(TAG, "Wi-Fi lock release without WAKE_LOCK: " + e.getClass().getName());
        }
    }

    private void storage() {
        StorageManager storage = (StorageManager) getSystemService(Context.STORAGE_SERVICE);
        Log.i(TAG, "storage service: " + storage.getClass().getName());
        List<StorageVolume> volumes = storage.getStorageVolumes();
        Log.i(TAG, "storage volumes: " + volumes.size());
        StorageVolume volume = volumes.get(0);
        Log.i(TAG, "volume: " + volume.getDescription(this) + ", directory " + volume.getDirectory() + ", primary "
                + volume.isPrimary() + ", removable " + volume.isRemovable() + ", emulated " + volume.isEmulated()
                + ", state " + volume.getState());
        Log.i(TAG, "volume: uuid " + volume.getUuid() + ", storage uuid " + volume.getStorageUuid()
                + ", media store volume " + volume.getMediaStoreVolumeName());
        Log.i(TAG, "the app's external files on it: "
                + volume.equals(storage.getStorageVolume(getExternalFilesDir(null))) + ", the primary volume "
                + volume.equals(storage.getPrimaryStorageVolume()));
        Log.i(TAG, "legacy external storage: " + Environment.isExternalStorageLegacy()
                + ", external storage manager: " + Environment.isExternalStorageManager());
    }

    private void appOps() {
        AppOpsManager appOps = (AppOpsManager) getSystemService(Context.APP_OPS_SERVICE);
        Log.i(TAG, "app ops service: " + appOps.getClass().getName());
        int uid = Process.myUid();
        String name = getPackageName();
        StringBuilder modes = new StringBuilder("app op modes:");
        // Written out where the op's name is not public API.
        for (String op : new String[] {AppOpsManager.OPSTR_CAMERA, AppOpsManager.OPSTR_RECORD_AUDIO,
                "android:manage_external_storage", AppOpsManager.OPSTR_PICTURE_IN_PICTURE,
                "android:no_isolated_storage"}) {
            modes.append(' ').append(op).append(' ').append(appOps.unsafeCheckOpNoThrow(op, uid, name));
        }
        Log.i(TAG, modes.toString());
        Log.i(TAG, "camera noted: " + appOps.noteOpNoThrow(AppOpsManager.OPSTR_CAMERA, uid, name, null, null));
        try {
            Log.i(TAG, "no isolated storage checked: "
                    + appOps.unsafeCheckOp("android:no_isolated_storage", uid, name));
        } catch (SecurityException e) {
            Log.i(TAG, "no isolated storage checked: " + e.getClass().getName());
        }
        guarded("an unknown op", () -> Log.i(TAG, "an unknown op: "
                + appOps.unsafeCheckOpNoThrow("android:no_such_op", uid, name)));
    }

    private void activity() {
        ActivityManager am = (ActivityManager) getSystemService(Context.ACTIVITY_SERVICE);
        Log.i(TAG, "activity service: " + am.getClass().getName() + ", the same by class "
                + (am == getSystemService(ActivityManager.class)));
        Log.i(TAG, "low-RAM device " + am.isLowRamDevice() + ", background restricted " + am.isBackgroundRestricted()
                + ", user test harness " + ActivityManager.isRunningInUserTestHarness() + ", monkey "
                + ActivityManager.isUserAMonkey());
        ActivityManager.MemoryInfo memory = new ActivityManager.MemoryInfo();
        am.getMemoryInfo(memory);
        Log.i(TAG, "memory info: available within total " + (memory.availMem > 0 && memory.availMem <= memory.totalMem)
                + ", threshold below available " + (memory.threshold < memory.availMem) + ", low " + memory.lowMemory);
        Log.i(DEVICE, "memory: total " + memory.totalMem + ", available " + memory.availMem + ", threshold "
                + memory.threshold + ", class " + am.getMemoryClass() + ", large class " + am.getLargeMemoryClass()
                + ", GLES 0x" + Integer.toHexString(am.getDeviceConfigurationInfo().reqGlEsVersion));
        List<ActivityManager.RunningAppProcessInfo> processes = am.getRunningAppProcesses();
        ActivityManager.RunningAppProcessInfo first = processes.get(0);
        Log.i(TAG, "running processes: " + processes.size() + ", the first " + first.processName + ", this one "
                + (first.pid == Process.myPid()) + ", importance " + first.importance);
        ActivityManager.RunningAppProcessInfo mine = new ActivityManager.RunningAppProcessInfo();
        ActivityManager.getMyMemoryState(mine);
        Log.i(TAG, "my memory state: importance " + mine.importance + ", trim level " + mine.lastTrimLevel);
    }

    private void display() {
        DisplayManager displays = (DisplayManager) getSystemService(Context.DISPLAY_SERVICE);
        Log.i(TAG, "display service: " + displays.getClass().getName() + ", the same by class "
                + (displays == getSystemService(DisplayManager.class)));
        Display[] all = displays.getDisplays();
        Log.i(TAG, "displays: " + all.length + ", the first's ID " + all[0].getDisplayId() + ", presentation displays "
                + displays.getDisplays(DisplayManager.DISPLAY_CATEGORY_PRESENTATION).length);
        Display display = displays.getDisplay(Display.DEFAULT_DISPLAY);
        Log.i(TAG, "display 0: valid " + display.isValid() + ", state " + display.getState());
        Log.i(DEVICE, "display 0: " + display.getName() + ", " + display.getRefreshRate() + " Hz, "
                + display.getSupportedModes().length + " modes, flags 0x" + Integer.toHexString(display.getFlags())
                + ", HDR " + display.isHdr());
        DisplayManager.DisplayListener listener = new DisplayManager.DisplayListener() {
            @Override
            public void onDisplayAdded(int displayId) {
                Log.i(TAG, "display added " + displayId);
            }

            @Override
            public void onDisplayRemoved(int displayId) {
                Log.i(TAG, "display removed " + displayId);
            }

            @Override
            public void onDisplayChanged(int displayId) {
            }
        };
        displays.registerDisplayListener(listener, null);
        displays.unregisterDisplayListener(listener);
        Log.i(TAG, "display listener registered and unregistered");
    }

    private void power() {
        PowerManager power = (PowerManager) getSystemService(Context.POWER_SERVICE);
        Log.i(TAG, "power service: " + power.getClass().getName() + ", the same by class "
                + (power == getSystemService(PowerManager.class)));
        Log.i(TAG, "interactive " + power.isInteractive() + ", power save " + power.isPowerSaveMode()
                + ", device idle " + power.isDeviceIdleMode() + ", light idle " + power.isDeviceLightIdleMode()
                + ", ignoring battery optimizations " + power.isIgnoringBatteryOptimizations(getPackageName()));
        Log.i(TAG, "thermal status " + power.getCurrentThermalStatus() + ", sustained performance "
                + power.isSustainedPerformanceModeSupported() + ", proximity wake lock "
                + power.isWakeLockLevelSupported(PowerManager.PROXIMITY_SCREEN_OFF_WAKE_LOCK) + ", location mode "
                + power.getLocationPowerSaveMode());
        PowerManager.WakeLock lock = power.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, TAG + ":partial");
        try {
            lock.acquire();
            Log.i(TAG, "wake lock without WAKE_LOCK: held " + lock.isHeld());
        } catch (SecurityException e) {
            Log.i(TAG, "wake lock without WAKE_LOCK: " + e.getClass().getName() + ", held " + lock.isHeld());
        }
    }

    private void audio() {
        AudioManager audio = (AudioManager) getSystemService(Context.AUDIO_SERVICE);
        Log.i(TAG, "audio service: " + audio.getClass().getName() + ", the same by class "
                + (audio == getSystemService(AudioManager.class)));
        AudioManager.OnAudioFocusChangeListener focus = change -> Log.i(TAG, "focus change " + change);
        Log.i(TAG, "focus request " + audio.requestAudioFocus(focus, AudioManager.STREAM_MUSIC,
                AudioManager.AUDIOFOCUS_GAIN) + ", abandon " + audio.abandonAudioFocus(focus));
        Log.i(TAG, "mode " + audio.getMode() + ", ringer " + audio.getRingerMode() + ", music active "
                + audio.isMusicActive() + ", speakerphone " + audio.isSpeakerphoneOn() + ", microphone muted "
                + audio.isMicrophoneMute() + ", volume fixed " + audio.isVolumeFixed());
        Log.i(TAG, "music volume " + audio.getStreamVolume(AudioManager.STREAM_MUSIC) + " of "
                + audio.getStreamMaxVolume(AudioManager.STREAM_MUSIC) + ", from "
                + audio.getStreamMinVolume(AudioManager.STREAM_MUSIC));
        Log.i(DEVICE, "output: " + audio.getProperty(AudioManager.PROPERTY_OUTPUT_SAMPLE_RATE) + " Hz, "
                + audio.getProperty(AudioManager.PROPERTY_OUTPUT_FRAMES_PER_BUFFER) + " frames per buffer");
    }

    private void connectivity() {
        ConnectivityManager net = (ConnectivityManager) getSystemService(Context.CONNECTIVITY_SERVICE);
        Log.i(TAG, "connectivity service: " + net.getClass().getName() + ", the same by class "
                + (net == getSystemService(ConnectivityManager.class)));
        // The network differs by device (the AVD's emulated modem, the iPhone's Wi-Fi), so its state is shown only.
        Network active = net.getActiveNetwork();
        Log.i(DEVICE, "active network " + active + ", its info " + net.getActiveNetworkInfo()
                + ", its capabilities " + net.getNetworkCapabilities(active) + ", networks "
                + net.getAllNetworks().length + ", metered " + net.isActiveNetworkMetered()
                + ", restrict background " + net.getRestrictBackgroundStatus());
        ConnectivityManager.NetworkCallback callback = new ConnectivityManager.NetworkCallback() {
            @Override
            public void onAvailable(Network network) {
                Log.i(TAG, "default network available");
            }
        };
        net.registerDefaultNetworkCallback(callback);
        net.unregisterNetworkCallback(callback);
        Log.i(TAG, "default network callback registered and unregistered");
    }

    private void user() {
        UserManager users = (UserManager) getSystemService(Context.USER_SERVICE);
        Log.i(TAG, "user service: " + users.getClass().getName() + ", the same by class "
                + (users == getSystemService(UserManager.class)));
        UserHandle me = Process.myUserHandle();
        Log.i(TAG, "user " + me + ", unlocked " + users.isUserUnlocked() + ", for this user " + users.isUserUnlocked(me)
                + ", system user " + users.isSystemUser() + ", demo " + users.isDemoUser() + ", managed profile "
                + users.isManagedProfile() + ", a profile " + users.isProfile() + ", a goat " + users.isUserAGoat());
        List<UserHandle> profiles = users.getUserProfiles();
        Log.i(TAG, "profiles: " + profiles.size() + ", the first " + profiles.get(0) + ", serial number "
                + users.getSerialNumberForUser(me) + ", the user of serial 0 " + users.getUserForSerialNumber(0));
        Log.i(TAG, "installing apps restricted " + users.hasUserRestriction(UserManager.DISALLOW_INSTALL_APPS)
                + ", application restrictions " + users.getApplicationRestrictions(getPackageName()).size());
        Log.i(TAG, "running " + users.isUserRunning(me) + ", quiet mode " + users.isQuietModeEnabled(me));
        // What the device's build and owner set: the user's restrictions and the multi-user support.
        Log.i(DEVICE, "restrictions " + new TreeSet<>(users.getUserRestrictions().keySet())
                + ", supports multiple users " + UserManager.supportsMultipleUsers());
        // A refusal's message names the app's uid, so only its class is logged.
        try {
            Log.i(TAG, "user name: \"" + users.getUserName() + "\"");
        } catch (SecurityException e) {
            Log.i(TAG, "user name: " + e.getClass().getName());
        }
        try {
            Log.i(TAG, "user count: " + users.getUserCount());
        } catch (SecurityException e) {
            Log.i(TAG, "user count: " + e.getClass().getName());
        }
        Log.i(DEVICE, "creation time " + users.getUserCreationTime(me));
    }

    private void notifications() {
        NotificationManager notes = (NotificationManager) getSystemService(Context.NOTIFICATION_SERVICE);
        Log.i(TAG, "notification service: " + notes.getClass().getName() + ", the same by class "
                + (notes == getSystemService(NotificationManager.class)));
        Log.i(TAG, "notifications enabled " + notes.areNotificationsEnabled() + ", importance " + notes.getImportance()
                + ", paused " + notes.areNotificationsPaused() + ", policy access "
                + notes.isNotificationPolicyAccessGranted() + ", interruption filter "
                + notes.getCurrentInterruptionFilter());
        notes.createNotificationChannelGroup(new NotificationChannelGroup("news", "News"));
        NotificationChannel updates = new NotificationChannel("updates", "Updates",
                NotificationManager.IMPORTANCE_HIGH);
        updates.setDescription("What changed");
        updates.setGroup("news");
        notes.createNotificationChannel(updates);
        // The service keeps a copy: what the app changes afterwards stays with the app's object.
        updates.setName("Changed here only");
        Log.i(TAG, "channel " + channel(notes.getNotificationChannel("updates")));
        Log.i(TAG, "the app's object: " + updates.getName() + ", lock screen " + updates.getLockscreenVisibility());
        NotificationChannel higher = new NotificationChannel("updates", "Updates 2",
                NotificationManager.IMPORTANCE_MAX);
        higher.setDescription("Changed");
        notes.createNotificationChannel(higher);
        Log.i(TAG, "created again, higher: " + channel(notes.getNotificationChannel("updates")));
        notes.createNotificationChannel(new NotificationChannel("updates", "Updates 3",
                NotificationManager.IMPORTANCE_LOW));
        Log.i(TAG, "created again, lower: " + channel(notes.getNotificationChannel("updates")));
        notes.createNotificationChannel(new NotificationChannel("other", "Other",
                NotificationManager.IMPORTANCE_DEFAULT));
        Log.i(TAG, "channels " + ids(notes.getNotificationChannels()) + ", unknown "
                + notes.getNotificationChannel("none"));
        for (NotificationChannelGroup group : notes.getNotificationChannelGroups()) {
            Log.i(TAG, "group " + group.getId() + " \"" + group.getName() + "\", blocked " + group.isBlocked()
                    + ", channels " + ids(group.getChannels()));
        }
        NotificationChannelGroup news = notes.getNotificationChannelGroup("news");
        Log.i(TAG, "group news by its ID: channels " + ids(news.getChannels()));
        Log.i(TAG, "in a group that does not exist: " + refusal(() -> {
            NotificationChannel lost = new NotificationChannel("lost", "Lost", NotificationManager.IMPORTANCE_LOW);
            lost.setGroup("none");
            notes.createNotificationChannel(lost);
        }));
        Log.i(TAG, "the reserved ID: " + refusal(() -> notes.createNotificationChannel(new NotificationChannel(
                NotificationChannel.DEFAULT_CHANNEL_ID, "Default", NotificationManager.IMPORTANCE_LOW))));
        Log.i(TAG, "importance 6: " + refusal(() -> notes.createNotificationChannel(
                new NotificationChannel("loud", "Loud", 6))));
        notes.deleteNotificationChannel("other");
        Log.i(TAG, "deleted: other " + notes.getNotificationChannel("other") + ", channels "
                + ids(notes.getNotificationChannels()));
        notes.createNotificationChannel(new NotificationChannel("other", "Other again",
                NotificationManager.IMPORTANCE_HIGH));
        Log.i(TAG, "created after its deletion: " + channel(notes.getNotificationChannel("other")));
        notes.deleteNotificationChannelGroup("news");
        Log.i(TAG, "group news deleted: groups " + notes.getNotificationChannelGroups().size() + ", updates "
                + notes.getNotificationChannel("updates") + ", channels " + ids(notes.getNotificationChannels()));
        Notification note = new Notification.Builder(this, "other").setSmallIcon(android.R.drawable.ic_dialog_info)
                .setContentTitle("Hello").setContentText("World").build();
        Log.i(TAG, "notification: channel " + note.getChannelId() + ", title "
                + note.extras.getCharSequence(Notification.EXTRA_TITLE) + ", text "
                + note.extras.getCharSequence(Notification.EXTRA_TEXT) + ", small icon type "
                + note.getSmallIcon().getType() + ", priority " + note.priority + ", visibility " + note.visibility
                + ", flags " + note.flags);
        notes.notify(1, note);
        notes.notify("tagged", 2, note);
        Log.i(TAG, "posted: active " + notes.getActiveNotifications().length);
        Log.i(TAG, "without a small icon: " + refusal(() -> notes.notify(3,
                new Notification.Builder(this, "other").setContentTitle("No icon").build())));
        notes.cancel(1);
        notes.cancel("tagged", 2);
        notes.cancelAll();
        Log.i(TAG, "cancelled: active " + notes.getActiveNotifications().length);
    }

    private static String channel(NotificationChannel channel) {
        return channel.getId() + " \"" + channel.getName() + "\" (" + channel.getDescription() + "), importance "
                + channel.getImportance() + ", group " + channel.getGroup() + ", sound " + channel.getSound()
                + ", usage " + channel.getAudioAttributes().getUsage() + ", vibration " + channel.shouldVibrate()
                + ", lights " + channel.shouldShowLights() + ", badge " + channel.canShowBadge() + ", bypass DND "
                + channel.canBypassDnd() + ", lock screen " + channel.getLockscreenVisibility() + ", bubbles "
                + channel.canBubble() + ", user set " + channel.hasUserSetImportance();
    }

    private static TreeSet<String> ids(List<NotificationChannel> channels) {
        TreeSet<String> ids = new TreeSet<>();
        for (NotificationChannel channel : channels) {
            ids.add(channel.getId());
        }
        return ids;
    }

    /** What a call throws, its class and message, or "fine". */
    private static String refusal(Section section) {
        try {
            section.run();
            return "fine";
        } catch (Exception e) {
            return e.getClass().getName() + ": " + e.getMessage();
        }
    }

    /** Runs a section; what it throws is logged with its name. */
    private static void guarded(String name, Section section) {
        try {
            section.run();
        } catch (Throwable e) {
            Log.i(TAG, name + ": " + e);
        }
    }
}
