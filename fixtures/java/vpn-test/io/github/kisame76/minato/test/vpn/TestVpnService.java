package io.github.kisame76.minato.test.vpn;

import android.content.Intent;
import android.net.VpnService;
import android.os.ParcelFileDescriptor;
import android.util.Log;

/** The app's own VpnService, which logs its life and asks for a tunnel it has not been allowed to have. */
public class TestVpnService extends VpnService {
    static final String TAG = "vpn-test";

    @Override
    public void onCreate() {
        super.onCreate();
        Log.i(TAG, "service: onCreate");
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        Log.i(TAG, "service: onStartCommand");
        applications();
        try {
            ParcelFileDescriptor tunnel = new Builder().setSession("vpn-test").addAddress("10.0.0.2", 32)
                .addRoute("0.0.0.0", 0).setMtu(1400).establish();
            Log.i(TAG, "service: establish " + (tunnel == null ? "returns null" : "returns a descriptor"));
            if (tunnel != null) tunnel.close();
        } catch (Throwable t) {
            Log.i(TAG, "service: establish throws " + t.getClass().getName());
        }
        Log.i(TAG, "service: protect(-1) " + protectResult());
        VpnApplication.serviceStarted();
        return START_NOT_STICKY;
    }

    private interface Step {
        void run() throws Exception;
    }

    private static String outcome(Step step) {
        try {
            step.run();
            return "ok";
        } catch (Throwable t) {
            return t.getClass().getName();
        }
    }

    /** The Builder's application lists: a package must be installed, and the two lists exclude each other. */
    private void applications() {
        final String own = getPackageName();
        Log.i(TAG, "builder allowed own: " + outcome(() -> new Builder().addAllowedApplication(own)));
        Log.i(TAG, "builder allowed missing: "
            + outcome(() -> new Builder().addAllowedApplication("io.github.kisame76.minato.test.absent")));
        Log.i(TAG, "builder disallowed own: " + outcome(() -> new Builder().addDisallowedApplication(own)));
        Log.i(TAG, "builder disallowed missing: "
            + outcome(() -> new Builder().addDisallowedApplication("io.github.kisame76.minato.test.absent")));
        Log.i(TAG, "builder allowed then disallowed: " + outcome(() -> {
            Builder builder = new Builder().addAllowedApplication(own);
            builder.addDisallowedApplication(own);
        }));
        Log.i(TAG, "builder disallowed then allowed: " + outcome(() -> {
            Builder builder = new Builder().addDisallowedApplication(own);
            builder.addAllowedApplication(own);
        }));
    }

    private String protectResult() {
        try {
            return String.valueOf(protect(-1));
        } catch (Throwable t) {
            return "throws " + t.getClass().getName();
        }
    }

    @Override
    public void onRevoke() {
        Log.i(TAG, "service: onRevoke");
        super.onRevoke();
    }

    @Override
    public void onDestroy() {
        Log.i(TAG, "service: onDestroy");
        super.onDestroy();
        VpnApplication.serviceDestroyed();
    }
}
