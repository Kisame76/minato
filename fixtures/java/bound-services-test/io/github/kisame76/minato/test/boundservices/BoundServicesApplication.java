package io.github.kisame76.minato.test.boundservices;

import android.app.Application;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.util.Log;

import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * The app's own bound services (ADR-0045 step SV3). Each part goes on from a callback (posted, so that the callback
 * returns first). Where Android does not fix the order of a connection's callbacks and the service's, as a binding
 * that dies under a stop, the part waits for all of them and logs them at once.
 */
public class BoundServicesApplication extends Application {
    static final String TAG = "bound-services-test";
    static final String OTHER = "io.github.kisame76.minato.test.boundservices.OTHER";

    static BoundServicesApplication sApp;
    private Handler mHandler;
    private int mPart;
    /** The events parts 6 and 11 wait for, whatever their order. */
    private int mWaiting;
    private final StringBuilder mSeen = new StringBuilder();
    private ExecutorService mExecutor;

    static void log(String line) {
        Log.i(TAG, line);
    }

    static boolean main() {
        return Looper.myLooper() == Looper.getMainLooper();
    }

    /** A connection that logs its callbacks under its name, and then lets its part go on. */
    final class Conn implements ServiceConnection {
        final String mName;
        final boolean mQuiet;
        IBinder mBinder;

        Conn(String name, boolean quiet) {
            mName = name;
            mQuiet = quiet;
        }

        @Override
        public void onServiceConnected(ComponentName name, IBinder service) {
            mBinder = service;
            String line = mName + " connected " + name.flattenToShortString() + " same binder "
                    + (service == BoundService.sBinder) + " main " + main() + " thread "
                    + (main() ? "main" : Thread.currentThread().getName());
            if (mQuiet) {
                seen(mName + " connected");
            } else {
                log(line);
                next();
            }
        }

        @Override
        public void onServiceDisconnected(ComponentName name) {
            seen(mName + " disconnected");
        }

        @Override
        public void onBindingDied(ComponentName name) {
            seen(mName + " died");
        }

        @Override
        public void onNullBinding(ComponentName name) {
            log(mName + " null binding " + name.flattenToShortString() + " main " + main());
            next();
        }

        @Override
        public String toString() {
            return "Conn " + mName;
        }
    }

    private final Conn mA = new Conn("A", false);
    private final Conn mB = new Conn("B", false);
    private final Conn mC = new Conn("C", false);
    private final Conn mD = new Conn("D", false);
    private final Conn mE = new Conn("E", true);
    private final Conn mF = new Conn("F", false);
    private final Conn mG = new Conn("G", false);
    private final Conn mH = new Conn("H", false);

    @Override
    public void onCreate() {
        super.onCreate();
        sApp = this;
        mHandler = new Handler(Looper.getMainLooper());
        mExecutor = Executors.newSingleThreadExecutor(r -> new Thread(r, "binder-executor"));
        next();
    }

    /** Runs the next part once the callback that asks for it returned. */
    static void next() {
        sApp.mHandler.post(() -> sApp.part(++sApp.mPart));
    }

    /** Whether a part waits for events in any order. */
    static boolean waiting() {
        return sApp.mWaiting > 0;
    }

    /** An event a part waits for in any order: once `mWaiting` of them came, they are logged sorted, and it goes on. */
    static void seen(String event) {
        sApp.mHandler.post(() -> {
            sApp.mSeen.append(sApp.mSeen.length() == 0 ? "" : ", ").append(event);
            if (--sApp.mWaiting == 0) {
                String[] events = sApp.mSeen.toString().split(", ");
                java.util.Arrays.sort(events);
                log("events: " + String.join(", ", events));
                sApp.mSeen.setLength(0);
                next();
            }
        });
    }

    private Intent bound() {
        return new Intent(this, BoundService.class);
    }

    private void unbind(Conn conn) {
        try {
            unbindService(conn);
            log("unbind " + conn.mName + ": done");
        } catch (IllegalArgumentException e) {
            log("unbind " + conn.mName + ": " + e.getClass().getName() + ": " + e.getMessage());
        }
    }

    private void part(int part) {
        switch (part) {
            case 1:
                // What answers at once: an implicit intent, no connection, another package, a missing service.
                try {
                    bindService(new Intent(OTHER), mA, Context.BIND_AUTO_CREATE);
                    log("implicit: bound");
                } catch (IllegalArgumentException e) {
                    log("implicit: " + e.getClass().getName() + ": " + e.getMessage());
                }
                try {
                    bindService(bound(), null, Context.BIND_AUTO_CREATE);
                    log("no connection: bound");
                } catch (IllegalArgumentException e) {
                    log("no connection: " + e.getClass().getName() + ": " + e.getMessage());
                }
                Conn other = new Conn("other", false);
                log("other package: " + bindService(new Intent().setClassName("io.github.kisame76.minato.test.absent",
                        "io.github.kisame76.minato.test.absent.Service"), other, Context.BIND_AUTO_CREATE));
                unbind(other);
                Conn missing = new Conn("missing", false);
                log("missing: " + bindService(new Intent().setClassName(this, getPackageName() + ".Missing"), missing,
                        Context.BIND_AUTO_CREATE));
                unbind(missing);
                unbind(new Conn("never", false));
                // One service, made for the first binding, which A gets once onBind returned.
                log("bind A: " + bindService(bound(), mA, Context.BIND_AUTO_CREATE));
                break;
            case 2:
                // The same intent again: no second onBind, B gets the binder A got.
                log("bind B: " + bindService(bound(), mB, Context.BIND_AUTO_CREATE));
                break;
            case 3:
                // Another intent: onBind again.
                log("bind C: " + bindService(bound().setAction(OTHER), mC, Context.BIND_AUTO_CREATE));
                break;
            case 4:
                // A's unbind leaves B, so no onUnbind comes before the next part.
                unbind(mA);
                unbind(mA);
                next();
                break;
            case 5:
                // B's unbinds the first intent; C's the second, and the service goes.
                unbind(mB);
                unbind(mC);
                break;
            case 6:
                // A started service bound by D, then unbound (onUnbind asks for onRebind) and bound by E: E gets the
                // binder at once and the service onRebind, in no fixed order.
                log("start: " + startService(bound()).flattenToShortString());
                log("bind D: " + bindService(bound(), mD, Context.BIND_AUTO_CREATE));
                break;
            case 7:
                mWaiting = 3;
                unbind(mD);
                log("bind E: " + bindService(bound(), mE, Context.BIND_AUTO_CREATE));
                break;
            case 8:
                // Stopped while bound with BIND_AUTO_CREATE: it stays until the binding goes.
                log("stop while bound: " + stopService(bound()));
                unbind(mE);
                break;
            case 9:
                // onBind's null.
                log("bind F: " + bindService(new Intent(this, NullService.class), mF, Context.BIND_AUTO_CREATE));
                break;
            case 10:
                unbind(mF);
                break;
            case 11:
                // A binding without BIND_AUTO_CREATE waits for a start; a stop then ends the service under it, which
                // the connection hears as a death, in no fixed order with the service's onUnbind and onDestroy.
                log("bind G: " + bindService(bound(), mG, 0));
                log("start: " + startService(bound()).flattenToShortString());
                break;
            case 12:
                mWaiting = 4;
                log("stop under G: " + stopService(bound()));
                break;
            case 13:
                unbind(mG);
                // A connection on an executor.
                log("bind H: " + bindService(bound(), Context.BIND_AUTO_CREATE, mExecutor, mH));
                break;
            case 14:
                unbind(mH);
                break;
            default:
                log("done");
                break;
        }
    }
}
