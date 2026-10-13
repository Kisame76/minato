package io.github.kisame76.minato.test.notedappop;

import android.app.Application;
import android.app.AppOpsManager;
import android.app.AsyncNotedAppOp;
import android.app.SyncNotedAppOp;
import android.os.Parcel;
import android.util.Log;

/**
 * The test of the app op notes, without a window (framework breadth, step (e)). It logs under the tag
 * noted-app-op-test what SyncNotedAppOp makes of an op number and an attribution tag (the op's name, the tag, equality,
 * a Parcel round trip, what a number outside the table meets), and what AsyncNotedAppOp reads back from a Parcel
 * written as its generated code does: op, noting uid, tag, message and time. Public API only (the Async parcel is the
 * app's input, as the system's service would send it). tools/android-reference.sh takes the log from Android 15, and
 * tools/noted-app-op-check.sh compares Minato's with it.
 */
public class NotedAppOpApplication extends Application {
    private static final String TAG = "noted-app-op-test";

    private interface Body {
        void run() throws Exception;
    }

    private static void probe(String name, Body body) {
        try {
            body.run();
        } catch (Throwable t) {
            Log.i(TAG, name + " failed: " + t.getClass().getName());
        }
    }

    @Override
    public void onCreate() {
        super.onCreate();
        probe("sync", () -> {
            int[] ops = {0, 26, 27, 148};
            for (int op : ops) {
                SyncNotedAppOp noted = new SyncNotedAppOp(op, op == 26 ? "feature" : null);
                Log.i(TAG, "sync " + op + " " + noted.getOp() + " tag=" + noted.getAttributionTag()
                        + " describe=" + noted.describeContents());
            }
            SyncNotedAppOp one = new SyncNotedAppOp(26, "feature");
            SyncNotedAppOp same = new SyncNotedAppOp(26, "feature");
            SyncNotedAppOp other = new SyncNotedAppOp(27, "feature");
            Log.i(TAG, "equal " + one.equals(same) + " " + one.equals(other) + " " + (one.hashCode() == same.hashCode()));
            Log.i(TAG, "string " + one);
            Parcel parcel = Parcel.obtain();
            try {
                one.writeToParcel(parcel, 0);
                parcel.setDataPosition(0);
                SyncNotedAppOp back = SyncNotedAppOp.CREATOR.createFromParcel(parcel);
                Log.i(TAG, "parcel " + back.getOp() + " " + back.getAttributionTag() + " " + back.equals(one) + " "
                        + SyncNotedAppOp.CREATOR.newArray(2).length);
            } finally {
                parcel.recycle();
            }
        });
        probe("syncBad", () -> {
            for (int op : new int[] {-1, 149, 1000}) {
                try {
                    Log.i(TAG, "bad " + op + " " + new SyncNotedAppOp(op, null).getOp());
                } catch (Throwable t) {
                    Log.i(TAG, "bad " + op + " " + t.getClass().getName());
                }
            }
        });
        probe("async", () -> {
            Parcel parcel = Parcel.obtain();
            try {
                parcel.writeByte((byte) 0x4);
                parcel.writeInt(AppOpsManager.strOpToOp(AppOpsManager.OPSTR_CAMERA));
                parcel.writeInt(10123);
                parcel.writeString("feature");
                parcel.writeString("noted by a test");
                parcel.writeLong(1234567890123L);
                parcel.setDataPosition(0);
                AsyncNotedAppOp noted = AsyncNotedAppOp.CREATOR.createFromParcel(parcel);
                Log.i(TAG, "async " + noted.getOp() + " uid=" + noted.getNotingUid() + " tag=" + noted.getAttributionTag()
                        + " message=" + noted.getMessage() + " time=" + noted.getTime()
                        + " describe=" + noted.describeContents());
                Parcel out = Parcel.obtain();
                try {
                    noted.writeToParcel(out, 0);
                    out.setDataPosition(0);
                    AsyncNotedAppOp back = AsyncNotedAppOp.CREATOR.createFromParcel(out);
                    Log.i(TAG, "async parcel " + back.getOp() + " " + back.getNotingUid() + " " + back.getMessage() + " "
                            + back.getTime() + " " + (back.toString().equals(noted.toString())) + " "
                            + AsyncNotedAppOp.CREATOR.newArray(3).length);
                } finally {
                    out.recycle();
                }
            } finally {
                parcel.recycle();
            }
        });
        probe("asyncNoTag", () -> {
            Parcel parcel = Parcel.obtain();
            try {
                parcel.writeByte((byte) 0);
                parcel.writeInt(0);
                parcel.writeInt(0);
                parcel.writeString("m");
                parcel.writeLong(0L);
                parcel.setDataPosition(0);
                AsyncNotedAppOp noted = AsyncNotedAppOp.CREATOR.createFromParcel(parcel);
                Log.i(TAG, "async " + noted.getOp() + " tag=" + noted.getAttributionTag());
            } finally {
                parcel.recycle();
            }
        });
        Log.i(TAG, "done");
    }
}
