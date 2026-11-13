/*
 * The battery: what system_server's BatteryService tells an app about the battery,
 * kept in the app's process from the host's battery (APKRT_EVENT_BATTERY and APKRT_NR_BATTERY, which the host feeds
 * from UIDevice's batteryLevel and batteryState). As BatteryService.processValuesLocked at android-15.0.0_r1 does on
 * each change of the battery: the power connected or disconnected broadcast when the plug changes, the battery low
 * broadcast when an unplugged battery falls to the warning level (config_lowBatteryWarningLevel), the battery okay
 * one when it is back at the warning level plus config_lowBatteryCloseWarningBump, then the sticky
 * ACTION_BATTERY_CHANGED with BatteryService's extras, through the local activity manager service, and the same
 * sequence number in all of them. The host's state gives the status and the plug: unplugged discharging, charging and
 * full on AC, as a phone's charger shows to Android, and an unknown state (the Simulator's) no battery present in an
 * unknown status. What iOS does not tell an app is a fixed value: health good, the technology Li-ion (an iPhone's
 * battery), and 0 for the voltage, the temperature, the charger's maximums, the charge counter and the cycle count.
 * It also answers BatteryManager: the local "batteryproperties" registrar gives the capacity and the status, and
 * reports the battery's other properties as unsupported, as a health HAL without them does; the local "batterystats"
 * service says whether the battery charges (charging or full) and that the times remaining are unknown.
 */
package android.app;

import android.content.Intent;
import android.content.res.Resources;
import android.os.BatteryManager;
import android.os.BatteryProperty;
import android.os.Binder;
import android.os.IBatteryPropertiesRegistrar;
import android.os.IBinder;

import com.android.internal.app.IBatteryStats;

public final class LocalBatteryService {
    /** APKRT_BATTERY_STATE_* of guest_interface.h: UIDevice.BatteryState's raw values. */
    private static final int STATE_UNPLUGGED = 1;
    private static final int STATE_CHARGING = 2;
    private static final int STATE_FULL = 3;
    /** BatteryService's BATTERY_SCALE. */
    private static final int SCALE = 100;
    private static final String TECHNOLOGY = "Li-ion";
    /** android.os.BatteryManager's hidden EXTRA_CHARGE_COUNTER. */
    private static final String EXTRA_CHARGE_COUNTER = "charge_counter";
    /** android.os.BatteryManager's hidden EXTRA_MAX_CHARGING_CURRENT and EXTRA_MAX_CHARGING_VOLTAGE. */
    private static final String EXTRA_MAX_CHARGING_CURRENT = "max_charging_current";
    private static final String EXTRA_MAX_CHARGING_VOLTAGE = "max_charging_voltage";
    /** android.os.BatteryManager's hidden EXTRA_INVALID_CHARGER. */
    private static final String EXTRA_INVALID_CHARGER = "invalid_charger";
    /** Binder's status for an unsupported property, as the health HAL's NAME_NOT_FOUND gives BatteryManager. */
    private static final int NAME_NOT_FOUND = -2;

    private static final Object sLock = new Object();
    /** Whether the battery is known: the host's first value, by event or at the first ask. */
    private static boolean sKnown;
    private static int sLevel;
    private static int sStatus;
    private static int sPlugType;
    private static boolean sPresent;
    private static int sSequence = 1;
    private static boolean sLevelLow;
    private static boolean sSentLowBroadcast;
    /** The low warning level and its close level, from framework-res's config at the first value. */
    private static int sLowLevel;
    private static int sCloseLevel;

    private LocalBatteryService() {
    }

    /** ServiceManager's maker for "batteryproperties": the Binder that carries the registrar. */
    public static IBinder newPropertiesBinder() {
        return new Properties().asBinder();
    }

    /** ServiceManager's maker for "batterystats": the Binder that carries the battery stats service. */
    public static IBinder newStatsBinder() {
        return new Stats().asBinder();
    }

    /**
     * ActivityThread's: the battery the host announced (APKRT_EVENT_BATTERY), `level` in percent or -1 when unknown.
     * Nothing changes for the same values, as BatteryService sends nothing then.
     */
    public static void onEvent(int level, int state) {
        update(level, state);
    }

    /** The battery as the host has it now, once: false while the host has given none. */
    private static boolean ensureKnown() {
        synchronized (sLock) {
            if (sKnown) {
                return true;
            }
        }
        final long battery = ActivityThread.battery();
        if (battery < 0) {
            return false;
        }
        update((int) battery, (int) (battery >>> 32));
        return true;
    }

    private static void update(int hostLevel, int state) {
        final Intent[] broadcasts;
        synchronized (sLock) {
            final int level = Math.max(hostLevel, 0);
            final int status = state == STATE_UNPLUGGED ? BatteryManager.BATTERY_STATUS_DISCHARGING
                    : state == STATE_CHARGING ? BatteryManager.BATTERY_STATUS_CHARGING
                    : state == STATE_FULL ? BatteryManager.BATTERY_STATUS_FULL
                    : BatteryManager.BATTERY_STATUS_UNKNOWN;
            final int plugType = state == STATE_CHARGING || state == STATE_FULL ? BatteryManager.BATTERY_PLUGGED_AC : 0;
            final boolean present = status != BatteryManager.BATTERY_STATUS_UNKNOWN;
            if (sKnown && level == sLevel && status == sStatus && plugType == sPlugType && present == sPresent) {
                return;
            }
            final boolean first = !sKnown;
            if (first) {
                final Resources resources = Resources.getSystem();
                sLowLevel = resources.getInteger(com.android.internal.R.integer.config_lowBatteryWarningLevel);
                sCloseLevel = sLowLevel
                        + resources.getInteger(com.android.internal.R.integer.config_lowBatteryCloseWarningBump);
            }
            final int lastLevel = first ? level : sLevel;
            final int lastPlugType = first ? plugType : sPlugType;
            sKnown = true;
            sLevel = level;
            sStatus = status;
            sPlugType = plugType;
            sPresent = present;
            broadcasts = processValues(first, lastLevel, lastPlugType);
        }
        final LocalActivityManagerService service = LocalActivityManagerService.instance();
        for (int i = 0; i < broadcasts.length - 1; i++) {
            if (broadcasts[i] != null) {
                service.broadcastFromSystem(broadcasts[i]);
            }
        }
        service.broadcastStickyFromSystem(broadcasts[broadcasts.length - 1]);
    }

    /**
     * BatteryService.processValuesLocked for a change: the low state, then the plug's and the low or okay broadcast
     * (null where none) and the sticky one, last. The first value is BatteryService's start, which has no last plug.
     */
    private static Intent[] processValues(boolean first, int lastLevel, int lastPlugType) {
        final boolean unplugged = sPlugType == 0;
        if (!sLevelLow) {
            if (unplugged && sStatus != BatteryManager.BATTERY_STATUS_UNKNOWN && sLevel <= sLowLevel) {
                sLevelLow = true;
            }
        } else if (!unplugged || sLevel >= sCloseLevel) {
            sLevelLow = false;
        }
        sSequence++;
        Intent plug = null;
        if (!first && sPlugType != 0 && lastPlugType == 0) {
            plug = statusIntent(Intent.ACTION_POWER_CONNECTED);
        } else if (!first && sPlugType == 0 && lastPlugType != 0) {
            plug = statusIntent(Intent.ACTION_POWER_DISCONNECTED);
        }
        Intent low = null;
        if (first) {
            // The process starts on a battery that the system has long told about: a low one was announced before
            // the app ran, so only the sticky one says it.
            sSentLowBroadcast = sLevelLow;
        } else if (unplugged && sStatus != BatteryManager.BATTERY_STATUS_UNKNOWN && sLevel <= sLowLevel
                && (lastPlugType != 0 || lastLevel > sLowLevel)) {
            // BatteryService.shouldSendBatteryLowLocked.
            sSentLowBroadcast = true;
            low = statusIntent(Intent.ACTION_BATTERY_LOW);
        } else if (sSentLowBroadcast && sLevel >= sCloseLevel) {
            sSentLowBroadcast = false;
            low = statusIntent(Intent.ACTION_BATTERY_OKAY);
        }
        return new Intent[] {plug, low, changedIntent()};
    }

    private static Intent statusIntent(String action) {
        final Intent intent = new Intent(action);
        intent.setFlags(Intent.FLAG_RECEIVER_REGISTERED_ONLY_BEFORE_BOOT);
        intent.putExtra(BatteryManager.EXTRA_SEQUENCE, sSequence);
        return intent;
    }

    /** BatteryService.sendBatteryChangedIntentLocked's intent. */
    private static Intent changedIntent() {
        final Intent intent = new Intent(Intent.ACTION_BATTERY_CHANGED);
        intent.addFlags(Intent.FLAG_RECEIVER_REGISTERED_ONLY | Intent.FLAG_RECEIVER_REPLACE_PENDING);
        intent.putExtra(BatteryManager.EXTRA_SEQUENCE, sSequence);
        intent.putExtra(BatteryManager.EXTRA_STATUS, sStatus);
        intent.putExtra(BatteryManager.EXTRA_HEALTH, BatteryManager.BATTERY_HEALTH_GOOD);
        intent.putExtra(BatteryManager.EXTRA_PRESENT, sPresent);
        intent.putExtra(BatteryManager.EXTRA_LEVEL, sLevel);
        intent.putExtra(BatteryManager.EXTRA_BATTERY_LOW, sSentLowBroadcast);
        intent.putExtra(BatteryManager.EXTRA_SCALE, SCALE);
        intent.putExtra(BatteryManager.EXTRA_ICON_SMALL, icon());
        intent.putExtra(BatteryManager.EXTRA_PLUGGED, sPlugType);
        intent.putExtra(BatteryManager.EXTRA_VOLTAGE, 0);
        intent.putExtra(BatteryManager.EXTRA_TEMPERATURE, 0);
        intent.putExtra(BatteryManager.EXTRA_TECHNOLOGY, TECHNOLOGY);
        intent.putExtra(EXTRA_INVALID_CHARGER, 0);
        intent.putExtra(EXTRA_MAX_CHARGING_CURRENT, 0);
        intent.putExtra(EXTRA_MAX_CHARGING_VOLTAGE, 0);
        intent.putExtra(EXTRA_CHARGE_COUNTER, 0);
        intent.putExtra(BatteryManager.EXTRA_CYCLE_COUNT, 0);
        intent.putExtra(BatteryManager.EXTRA_CHARGING_STATUS, 0);
        return intent;
    }

    /** BatteryService.getIconLocked. */
    private static int icon() {
        if (sStatus == BatteryManager.BATTERY_STATUS_CHARGING) {
            return com.android.internal.R.drawable.stat_sys_battery_charge;
        } else if (sStatus == BatteryManager.BATTERY_STATUS_DISCHARGING) {
            return com.android.internal.R.drawable.stat_sys_battery;
        } else if (sStatus == BatteryManager.BATTERY_STATUS_FULL) {
            return sPlugType != 0 && sLevel >= 100 ? com.android.internal.R.drawable.stat_sys_battery_charge
                    : com.android.internal.R.drawable.stat_sys_battery;
        }
        return com.android.internal.R.drawable.stat_sys_battery_unknown;
    }

    /** IBatteryPropertiesRegistrar of BatteryService's binder service: the capacity and the status. */
    private static final class Properties extends IBatteryPropertiesRegistrar.Default {
        private final Binder mBinder = new Binder();

        Properties() {
            mBinder.attachInterface(this, IBatteryPropertiesRegistrar.Stub.DESCRIPTOR);
        }

        @Override
        public IBinder asBinder() {
            return mBinder;
        }

        @Override
        public int getProperty(int id, BatteryProperty prop) {
            if (!ensureKnown()) {
                return NAME_NOT_FOUND;
            }
            synchronized (sLock) {
                switch (id) {
                    case BatteryManager.BATTERY_PROPERTY_CAPACITY:
                        prop.setLong(sLevel);
                        return 0;
                    case BatteryManager.BATTERY_PROPERTY_STATUS:
                        prop.setLong(sStatus);
                        return 0;
                    default:
                        return NAME_NOT_FOUND;
                }
            }
        }
    }

    /** IBatteryStats of BatteryStatsService, for what BatteryManager asks of it. */
    private static final class Stats extends IBatteryStats.Default {
        private final Binder mBinder = new Binder();

        Stats() {
            mBinder.attachInterface(this, IBatteryStats.Stub.DESCRIPTOR);
        }

        @Override
        public IBinder asBinder() {
            return mBinder;
        }

        @Override
        public boolean isCharging() {
            if (!ensureKnown()) {
                return false;
            }
            synchronized (sLock) {
                return sStatus == BatteryManager.BATTERY_STATUS_CHARGING
                        || sStatus == BatteryManager.BATTERY_STATUS_FULL;
            }
        }

        @Override
        public long computeBatteryTimeRemaining() {
            return -1;
        }

        @Override
        public long computeChargeTimeRemaining() {
            return -1;
        }

        @Override
        public boolean setChargingStateUpdateDelayMillis(int delay) {
            ActivityThread.currentApplication().enforceCallingOrSelfPermission(
                    android.Manifest.permission.POWER_SAVER, null);
            return false;
        }
    }

    /** LocalActivityManagerService's: the sticky ACTION_BATTERY_CHANGED before a filter looks for it. */
    static void prepareSticky() {
        ensureKnown();
    }
}
