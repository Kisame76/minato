package io.github.kisame76.minato.test.battery;

import android.app.Activity;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.BatteryManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/**
 * The battery (gap 1 of the compatibility plan), as apps and libraries read it (WorkManager's battery constraints,
 * game engines' power state, analytics):
 * <ul>
 *   <li>the sticky ACTION_BATTERY_CHANGED that registerReceiver gives for a null receiver: its flags, its extras' names
 *       and types, and the level, scale, status, plug and low flag of the battery the run starts with, 100 % charging
 *       on AC (tools/android-reference.sh sets it on the emulator, apk-run's fixed battery is the same). The
 *       battery's health, voltage, temperature, technology and counters differ by device and are on the device tag;
 *   <li>BatteryManager by name and by class, its capacity and status properties against the sticky's, and what
 *       differs by device: isCharging, the other properties and the charge time remaining;
 *   <li>a receiver of the battery's broadcasts, which gets the sticky one at once, and then, after "ready", the
 *       battery the run changes in steps: to 50 %, unplugged, to 10 %, plugged in, to 50 % again. Each step's
 *       ACTION_BATTERY_CHANGED is logged once its values are there; the emulator's console takes one value per
 *       command, so the states between (plugged but discharging) are not logged, as a phone's battery does not pass
 *       through them. The power connected and disconnected, battery low and okay broadcasts are logged as they come;
 *   <li>the sticky broadcast and BatteryManager's properties again at the end; the emulator's shell sets the
 *       broadcast's values but not the battery's properties, which it reads from the battery's HAL, so the
 *       properties against the broadcast's are on the device tag there.
 * </ul>
 * Logs under the app's label and, what differs by device, the label with "-device"; "done" at the end, also after
 * 60 s without the next step, with the step it waited for.
 */
public class BatteryActivity extends Activity {
    /** The steps' level, status and plug, as the run changes the battery. */
    private static final int[][] STEPS = {
        {50, BatteryManager.BATTERY_STATUS_CHARGING, BatteryManager.BATTERY_PLUGGED_AC},
        {50, BatteryManager.BATTERY_STATUS_DISCHARGING, 0},
        {10, BatteryManager.BATTERY_STATUS_DISCHARGING, 0},
        {10, BatteryManager.BATTERY_STATUS_CHARGING, BatteryManager.BATTERY_PLUGGED_AC},
        {50, BatteryManager.BATTERY_STATUS_CHARGING, BatteryManager.BATTERY_PLUGGED_AC},
    };
    private static final long TIMEOUT_MS = 60_000;

    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private String mTag;
    private String mDevice;
    private boolean mStarted;
    private boolean mDone;
    private int mStep;
    private BatteryManager mManager;

    private final BroadcastReceiver mReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            received(intent, isInitialStickyBroadcast());
        }
    };

    private final Runnable mTimeout = () -> {
        Log.i(mTag, "timeout at step " + (mStep + 1));
        end();
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        mTag = getApplicationInfo().loadLabel(getPackageManager()).toString();
        mDevice = mTag + "-device";
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (!hasFocus || mStarted) {
            return;
        }
        mStarted = true;
        Log.i(mTag, "start");
        final Intent sticky = registerReceiver(null, new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
        if (sticky == null) {
            Log.i(mTag, "sticky: null");
        } else {
            Log.i(mTag, "sticky: " + sticky.getAction() + ", flags 0x" + Integer.toHexString(sticky.getFlags()));
            Log.i(mTag, "sticky extras: " + extras(sticky));
            Log.i(mTag, "sticky: " + values(sticky));
            Log.i(mDevice, "sticky: health " + sticky.getIntExtra(BatteryManager.EXTRA_HEALTH, -1) + ", present "
                    + sticky.getBooleanExtra(BatteryManager.EXTRA_PRESENT, false) + ", voltage "
                    + sticky.getIntExtra(BatteryManager.EXTRA_VOLTAGE, -1) + ", temperature "
                    + sticky.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, -1) + ", technology "
                    + sticky.getStringExtra(BatteryManager.EXTRA_TECHNOLOGY) + ", charge counter "
                    + sticky.getIntExtra("charge_counter", -1) + ", cycle count "
                    + sticky.getIntExtra(BatteryManager.EXTRA_CYCLE_COUNT, -1) + ", charging status "
                    + sticky.getIntExtra(BatteryManager.EXTRA_CHARGING_STATUS, -1));
        }
        final Object byName = getSystemService(Context.BATTERY_SERVICE);
        mManager = getSystemService(BatteryManager.class);
        Log.i(mTag, "battery manager: " + (byName == null ? "null" : byName.getClass().getName())
                + ", the same by class " + (byName == mManager));
        properties(sticky, mTag);
        final IntentFilter filter = new IntentFilter(Intent.ACTION_BATTERY_CHANGED);
        filter.addAction(Intent.ACTION_POWER_CONNECTED);
        filter.addAction(Intent.ACTION_POWER_DISCONNECTED);
        filter.addAction(Intent.ACTION_BATTERY_LOW);
        filter.addAction(Intent.ACTION_BATTERY_OKAY);
        // Protected broadcasts only, which need no export flag.
        registerReceiver(mReceiver, filter);
        mHandler.postDelayed(mTimeout, TIMEOUT_MS);
        Log.i(mTag, "ready");
    }

    /** The extras' names with their types, sorted. */
    private static String extras(Intent intent) {
        final Bundle bundle = intent.getExtras();
        if (bundle == null) {
            return "none";
        }
        final List<String> names = new ArrayList<>();
        for (String key : bundle.keySet()) {
            final Object value = bundle.get(key);
            names.add(key + ":" + (value == null ? "null" : value.getClass().getSimpleName()));
        }
        Collections.sort(names);
        return String.join(", ", names);
    }

    private static String values(Intent intent) {
        return "level " + intent.getIntExtra(BatteryManager.EXTRA_LEVEL, -1) + ", scale "
                + intent.getIntExtra(BatteryManager.EXTRA_SCALE, -1) + ", status "
                + intent.getIntExtra(BatteryManager.EXTRA_STATUS, -1) + ", plugged "
                + intent.getIntExtra(BatteryManager.EXTRA_PLUGGED, -1) + ", low "
                + intent.getBooleanExtra(BatteryManager.EXTRA_BATTERY_LOW, false);
    }

    /** BatteryManager's properties: capacity and status against the sticky broadcast on `tag`, the rest by device. */
    private void properties(Intent sticky, String tag) {
        if (mManager == null) {
            return;
        }
        final int capacity = mManager.getIntProperty(BatteryManager.BATTERY_PROPERTY_CAPACITY);
        final int status = mManager.getIntProperty(BatteryManager.BATTERY_PROPERTY_STATUS);
        Log.i(tag, "capacity " + capacity + ", status " + status + ", as the sticky's " + (sticky != null
                && capacity == sticky.getIntExtra(BatteryManager.EXTRA_LEVEL, -1)
                && status == sticky.getIntExtra(BatteryManager.EXTRA_STATUS, -1)));
        Log.i(mDevice, "charging " + mManager.isCharging() + ", charge counter "
                + mManager.getIntProperty(BatteryManager.BATTERY_PROPERTY_CHARGE_COUNTER) + ", current now "
                + mManager.getIntProperty(BatteryManager.BATTERY_PROPERTY_CURRENT_NOW) + ", current average "
                + mManager.getIntProperty(BatteryManager.BATTERY_PROPERTY_CURRENT_AVERAGE) + ", energy counter "
                + mManager.getLongProperty(BatteryManager.BATTERY_PROPERTY_ENERGY_COUNTER) + ", charge time remaining "
                + mManager.computeChargeTimeRemaining());
    }

    private void received(Intent intent, boolean initial) {
        if (mDone) {
            return;
        }
        final String action = intent.getAction();
        if (!Intent.ACTION_BATTERY_CHANGED.equals(action)) {
            Log.i(mTag, "step " + (mStep + 1) + ": " + action + ", extras: " + extras(intent));
            return;
        }
        if (initial) {
            Log.i(mTag, "receiver: the sticky one, " + values(intent));
            return;
        }
        final int[] step = STEPS[mStep];
        if (intent.getIntExtra(BatteryManager.EXTRA_LEVEL, -1) != step[0]
                || intent.getIntExtra(BatteryManager.EXTRA_STATUS, -1) != step[1]
                || intent.getIntExtra(BatteryManager.EXTRA_PLUGGED, -1) != step[2]) {
            return;
        }
        Log.i(mTag, "step " + (mStep + 1) + ": " + values(intent));
        mStep++;
        mHandler.removeCallbacks(mTimeout);
        if (mStep == STEPS.length) {
            end();
        } else {
            mHandler.postDelayed(mTimeout, TIMEOUT_MS);
        }
    }

    private void end() {
        if (mDone) {
            return;
        }
        mDone = true;
        unregisterReceiver(mReceiver);
        final Intent sticky = registerReceiver(null, new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
        Log.i(mTag, "sticky at the end: " + (sticky == null ? "null" : values(sticky)));
        properties(sticky, mDevice);
        Log.i(mTag, "done");
        finish();
    }
}
