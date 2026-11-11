package io.github.kisame76.minato.test.vpn;

import android.app.Application;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.net.VpnService;
import android.os.IBinder;
import android.util.Log;

import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/**
 * The VpnService test (group 1 of the framework breadth). The user has not allowed this app to be a VPN: prepare()
 * hands back the consent dialog's intent every time, because the dialog is never started, the app's service can be
 * started and bound, and establish() gives no tunnel.
 */
public class VpnApplication extends Application {
    private static final String TAG = "vpn-test";
    private static final CountDownLatch STARTED = new CountDownLatch(1);

    private static final CountDownLatch ROUNDS = new CountDownLatch(1);
    private static final CountDownLatch DESTROYED = new CountDownLatch(1);

    static void serviceStarted() {
        STARTED.countDown();
    }

    static void serviceDestroyed() {
        DESTROYED.countDown();
    }

    @Override
    public void onCreate() {
        super.onCreate();
        new Thread(this::run).start();
    }

    private void run() {
        try {
            shape();
            prepare();
            service();
        } catch (Throwable t) {
            Log.i(TAG, "failed: " + t);
        }
        ROUNDS.countDown();
    }

    /** Blocks until the application's rounds are over (the activity's start follows them). */
    static void awaitRounds() {
        try {
            ROUNDS.await(60, TimeUnit.SECONDS);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }

    /** The last line: the activity's round is over. */
    static void finish() {
        Log.i(TAG, "done");
    }

    private static void shape() throws Exception {
        Class<?> vpn = Class.forName("android.net.VpnService");
        Log.i(TAG, "class: " + vpn.getName() + " extends " + vpn.getSuperclass().getName() + ", "
            + (Modifier.isAbstract(vpn.getModifiers()) ? "abstract" : "not abstract"));
        Log.i(TAG, "SERVICE_INTERFACE: " + VpnService.SERVICE_INTERFACE);
        String[] methods = {"prepare", "protect", "setUnderlyingNetworks", "isAlwaysOn", "isLockdownEnabled",
            "onRevoke", "onBind"};
        for (String name : methods) {
            int count = 0;
            StringBuilder returns = new StringBuilder();
            for (Method method : vpn.getMethods()) {
                if (method.getName().equals(name) && method.getDeclaringClass() == vpn) {
                    count++;
                    returns.append(' ').append(method.getReturnType().getSimpleName());
                }
            }
            Log.i(TAG, "method " + name + ": " + count + returns);
        }
        Class<?> builder = Class.forName("android.net.VpnService$Builder");
        String[] builderMethods = {"setSession", "setConfigureIntent", "addAddress", "addRoute", "addDnsServer",
            "addSearchDomain", "setMtu", "setBlocking", "setMetered", "setHttpProxy", "allowFamily",
            "addAllowedApplication", "addDisallowedApplication", "allowBypass", "setUnderlyingNetworks", "establish"};
        for (String name : builderMethods) {
            int count = 0;
            StringBuilder returns = new StringBuilder();
            for (Method method : builder.getMethods()) {
                if (method.getName().equals(name) && method.getDeclaringClass() == builder) {
                    count++;
                    returns.append(' ').append(method.getReturnType().getSimpleName());
                }
            }
            Log.i(TAG, "builder " + name + ": " + count + returns);
        }
    }

    private void prepare() {
        for (int i = 1; i <= 2; i++) {
            Intent intent = VpnService.prepare(this);
            if (intent == null) {
                Log.i(TAG, "prepare " + i + ": null");
            } else {
                ComponentName component = intent.getComponent();
                Log.i(TAG, "prepare " + i + ": intent for "
                    + (component == null ? "no component" : component.flattenToShortString()));
            }
        }
    }

    private void service() throws Exception {
        Intent start = new Intent(this, TestVpnService.class);
        Log.i(TAG, "startService: " + (startService(start) != null));
        Log.i(TAG, "started: " + STARTED.await(10, TimeUnit.SECONDS));
        CountDownLatch bound = new CountDownLatch(1);
        ServiceConnection connection = new ServiceConnection() {
            @Override
            public void onServiceConnected(ComponentName name, IBinder binder) {
                Log.i(TAG, "bind: connected, binder " + (binder == null ? "null" : "not null"));
                bound.countDown();
            }

            @Override
            public void onServiceDisconnected(ComponentName name) {
            }

            @Override
            public void onNullBinding(ComponentName name) {
                Log.i(TAG, "bind: null binding");
                bound.countDown();
            }
        };
        Intent bind = new Intent(VpnService.SERVICE_INTERFACE).setPackage(getPackageName());
        // The callbacks run on the main thread: what this thread logs waits for them, so that the order is fixed.
        boolean binding = bindService(bind, connection, Context.BIND_AUTO_CREATE);
        boolean connected = bound.await(10, TimeUnit.SECONDS);
        Log.i(TAG, "bindService: " + binding + ", connected: " + connected);
        unbindService(connection);
        boolean stopped = stopService(start);
        boolean destroyed = DESTROYED.await(10, TimeUnit.SECONDS);
        Log.i(TAG, "stopService: " + stopped + ", destroyed: " + destroyed);
    }
}
