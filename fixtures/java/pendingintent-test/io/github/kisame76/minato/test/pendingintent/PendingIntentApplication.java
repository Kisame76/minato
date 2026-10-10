package io.github.kisame76.minato.test.pendingintent;

import android.app.Application;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.Process;
import android.util.Log;

/**
 * PendingIntents (ADR-0045 step d3, batch D 3: AlarmManager and WorkManager hand the system PendingIntents): one
 * request gives the same PendingIntent again, by the Intent's filterEquals, the request code and the kind, and not by
 * the extras; FLAG_NO_CREATE finds, FLAG_UPDATE_CURRENT updates and FLAG_CANCEL_CURRENT replaces; a missing mutability
 * flag is refused; the creator; cancel; and sends of broadcasts to a receiver the app registers, with the extras of an
 * update, a mutable one's fill-in and an immutable one's without it, a one-shot sent twice, and an ordered send whose
 * receiver sets the result. "done" follows the last receipt.
 */
public class PendingIntentApplication extends Application {
    static final String TAG = "pendingintent-test";
    static final String ACTION = "io.github.kisame76.minato.test.pendingintent.ACTION";
    static final int SENDS = 6;

    private int mReceipts;
    private boolean mFinished;

    private static void log(String line) {
        Log.i(TAG, line);
    }

    private interface Action {
        Object run() throws Exception;
    }

    private static String outcome(Action action) {
        try {
            return String.valueOf(action.run());
        } catch (Throwable e) {
            return e.getClass().getName() + ": " + e.getMessage();
        }
    }

    private Intent intent(String action) {
        return new Intent(action).setPackage(getPackageName());
    }

    private PendingIntent broadcast(int code, Intent intent, int flags) {
        return PendingIntent.getBroadcast(this, code, intent, flags);
    }

    private void checkDone() {
        if (mReceipts == SENDS && mFinished) {
            log("done");
        }
    }

    @Override
    public void onCreate() {
        super.onCreate();
        int immutable = PendingIntent.FLAG_IMMUTABLE;
        PendingIntent first = broadcast(1, intent(ACTION + ".SAME").putExtra("n", 1), immutable);
        PendingIntent again = broadcast(1, intent(ACTION + ".SAME").putExtra("n", 9), immutable);
        PendingIntent otherCode = broadcast(2, intent(ACTION + ".SAME"), immutable);
        PendingIntent otherAction = broadcast(1, intent(ACTION + ".OTHER"), immutable);
        PendingIntent asActivity = PendingIntent.getActivity(this, 1, intent(ACTION + ".SAME"), immutable);
        PendingIntent asService = PendingIntent.getService(this, 1, intent(ACTION + ".SAME"), immutable);
        log("same request: " + first.equals(again) + ", same hash: " + (first.hashCode() == again.hashCode()));
        log("other request code: " + first.equals(otherCode) + ", other action: " + first.equals(otherAction)
                + ", as an activity: " + first.equals(asActivity) + ", as a service: " + first.equals(asService));
        log("kinds: broadcast " + first.isBroadcast() + " " + first.isActivity() + " " + first.isService()
                + ", activity " + asActivity.isActivity() + ", service " + asService.isService());
        log("creator: " + first.getCreatorPackage() + ", own uid " + (first.getCreatorUid() == Process.myUid())
                + ", user " + first.getCreatorUserHandle() + ", immutable " + first.isImmutable());
        log("no create, missing: " + broadcast(3, intent(ACTION + ".NONE"), immutable | PendingIntent.FLAG_NO_CREATE));
        log("no create, there: " + first.equals(broadcast(1, intent(ACTION + ".SAME"),
                immutable | PendingIntent.FLAG_NO_CREATE)));
        log("no mutability flag: " + outcome(() -> broadcast(4, intent(ACTION + ".FLAGS"), 0)));
        log("both mutability flags: " + outcome(() -> broadcast(4, intent(ACTION + ".FLAGS"),
                PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_MUTABLE)));

        PendingIntent replaced = broadcast(5, intent(ACTION + ".REPLACE"), immutable);
        PendingIntent replacing = broadcast(5, intent(ACTION + ".REPLACE"),
                immutable | PendingIntent.FLAG_CANCEL_CURRENT);
        log("cancel current: equal " + replaced.equals(replacing) + ", the old one sent: " + outcome(() -> {
            replaced.send();
            return "sent";
        }));
        PendingIntent cancelled = broadcast(6, intent(ACTION + ".CANCEL"), immutable);
        cancelled.cancel();
        log("cancelled: sent " + outcome(() -> {
            cancelled.send();
            return "sent";
        }) + ", found again " + broadcast(6, intent(ACTION + ".CANCEL"), immutable | PendingIntent.FLAG_NO_CREATE));
        log("cancel twice: " + outcome(() -> {
            cancelled.cancel();
            return "ok";
        }));

        registerReceiver(new BroadcastReceiver() {
            @Override
            public void onReceive(Context context, Intent received) {
                mReceipts++;
                String action = received.getAction().substring(ACTION.length() + 1);
                log("received " + action + ": n " + received.getIntExtra("n", 0) + ", fill-in "
                        + received.getStringExtra("f") + ", ordered " + isOrderedBroadcast());
                if (isOrderedBroadcast()) {
                    setResultCode(42);
                    setResultData("from the receiver");
                }
                checkDone();
            }
        }, filter(), Context.RECEIVER_NOT_EXPORTED);

        log("send: " + outcome(() -> {
            first.send();
            return "sent";
        }));
        PendingIntent updated = broadcast(1, intent(ACTION + ".SAME").putExtra("n", 2),
                immutable | PendingIntent.FLAG_UPDATE_CURRENT);
        log("update current: equal " + first.equals(updated) + ", sent " + outcome(() -> {
            updated.send();
            return "sent";
        }));
        PendingIntent mutable = broadcast(7, intent(ACTION + ".MUTABLE"), PendingIntent.FLAG_MUTABLE);
        log("mutable with a fill-in: " + outcome(() -> {
            mutable.send(this, 0, new Intent().putExtra("f", "x"));
            return "sent";
        }));
        PendingIntent fixed = broadcast(8, intent(ACTION + ".IMMUTABLE"), immutable);
        log("immutable with a fill-in: " + outcome(() -> {
            fixed.send(this, 0, new Intent().putExtra("f", "x"));
            return "sent";
        }));
        PendingIntent once = broadcast(9, intent(ACTION + ".ONCE"), immutable | PendingIntent.FLAG_ONE_SHOT);
        log("one shot: first " + outcome(() -> {
            once.send();
            return "sent";
        }) + ", second " + outcome(() -> {
            once.send();
            return "sent";
        }));
        PendingIntent ordered = broadcast(10, intent(ACTION + ".ORDERED"), immutable);
        log("ordered send: " + outcome(() -> {
            ordered.send(7, (pendingIntent, sent, resultCode, resultData, resultExtras) -> {
                log("finished: code " + resultCode + ", data " + resultData + ", same " + ordered.equals(
                        pendingIntent) + ", action " + sent.getAction().substring(ACTION.length() + 1));
                mFinished = true;
                checkDone();
            }, null);
            return "sent";
        }));
    }

    private static IntentFilter filter() {
        IntentFilter filter = new IntentFilter();
        for (String action : new String[] {"SAME", "MUTABLE", "IMMUTABLE", "ONCE", "ORDERED", "REPLACE", "CANCEL"}) {
            filter.addAction(ACTION + "." + action);
        }
        return filter;
    }
}
