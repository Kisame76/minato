package io.github.kisame76.minato.test.workmanager;

import android.app.Activity;
import android.os.Bundle;
import android.util.Log;
import androidx.work.Constraints;
import androidx.work.Data;
import androidx.work.ExistingPeriodicWorkPolicy;
import androidx.work.NetworkType;
import androidx.work.OneTimeWorkRequest;
import androidx.work.OutOfQuotaPolicy;
import androidx.work.PeriodicWorkRequest;
import androidx.work.WorkInfo;
import androidx.work.WorkManager;
import androidx.work.WorkRequest;
import java.util.List;
import java.util.concurrent.TimeUnit;

/** Asks WorkManager for one-time, failing, constrained, chained, periodic, delayed, expedited and kept work. */
public class WorkActivity extends Activity {
    static final String TAG = "workmanager-test";
    private WorkManager mWork;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        new Thread(this::run, "workmanager-test").start();
    }

    private void run() {
        try {
            mWork = WorkManager.getInstance(this);
            Log.i(TAG, "instance: " + (mWork != null));
            kept();
            oneTime();
            failing();
            constraints();
            chain();
            periodic();
            delayedCancel();
            tags();
            expedited();
        } catch (Throwable t) {
            Log.i(TAG, "failed: " + t);
        }
        Log.i(TAG, "done");
    }

    /**
     * Work that lasts beyond the process: the first start enqueues a delayed unique request, a later start, after the
     * process was killed in the background, finds it in WorkManager's database as it was.
     */
    private void kept() throws Exception {
        List<WorkInfo> kept = mWork.getWorkInfosForUniqueWork("kept").get();
        Log.i(TAG, "restart: kept work " + (kept.isEmpty() ? "none" : kept.size() + ", " + kept.get(0).getState()));
        if (kept.isEmpty()) {
            mWork.enqueueUniqueWork("kept", androidx.work.ExistingWorkPolicy.KEEP,
                    new OneTimeWorkRequest.Builder(CountWorker.class).setInitialDelay(1, TimeUnit.HOURS).build());
        }
    }

    private Data input(int n) {
        return new Data.Builder().putInt("n", n).build();
    }

    /** Waits up to `seconds` for the request to finish; returns its last info. */
    private WorkInfo await(WorkRequest request, int seconds) throws Exception {
        WorkInfo info = null;
        for (int i = 0; i < seconds * 10; i++) {
            info = mWork.getWorkInfoById(request.getId()).get();
            if (info != null && info.getState().isFinished()) {
                return info;
            }
            Thread.sleep(100);
        }
        return info;
    }

    private void oneTime() throws Exception {
        OneTimeWorkRequest request = new OneTimeWorkRequest.Builder(CountWorker.class).setInputData(input(1)).build();
        mWork.enqueue(request);
        WorkInfo info = await(request, 20);
        Log.i(TAG, "one-time: " + info.getState() + ", out " + info.getOutputData().getString("out"));
    }

    private void failing() throws Exception {
        OneTimeWorkRequest request = new OneTimeWorkRequest.Builder(FailWorker.class).build();
        mWork.enqueue(request);
        Log.i(TAG, "failing: " + await(request, 20).getState());
    }

    private void constraints() throws Exception {
        Constraints idle = new Constraints.Builder().setRequiresDeviceIdle(true).build();
        OneTimeWorkRequest blocked = new OneTimeWorkRequest.Builder(CountWorker.class).setConstraints(idle).build();
        mWork.enqueue(blocked);
        Thread.sleep(3000);
        Log.i(TAG, "constraint device idle: " + mWork.getWorkInfoById(blocked.getId()).get().getState());
        mWork.cancelWorkById(blocked.getId());
        Log.i(TAG, "constraint device idle, cancelled: " + await(blocked, 10).getState());
        Constraints network = new Constraints.Builder().setRequiredNetworkType(NetworkType.CONNECTED).build();
        OneTimeWorkRequest connected = new OneTimeWorkRequest.Builder(CountWorker.class).setConstraints(network)
                .setInputData(input(2)).build();
        mWork.enqueue(connected);
        Log.i(TAG, "constraint network: " + await(connected, 20).getState());
        Constraints battery = new Constraints.Builder().setRequiresBatteryNotLow(true).setRequiresStorageNotLow(true)
                .build();
        OneTimeWorkRequest low = new OneTimeWorkRequest.Builder(CountWorker.class).setConstraints(battery)
                .setInputData(input(3)).build();
        mWork.enqueue(low);
        Log.i(TAG, "constraint battery and storage not low: " + await(low, 20).getState());
    }

    private void chain() throws Exception {
        OneTimeWorkRequest first = new OneTimeWorkRequest.Builder(CountWorker.class).setInputData(input(4)).build();
        OneTimeWorkRequest second = new OneTimeWorkRequest.Builder(EchoWorker.class).build();
        mWork.beginWith(first).then(second).enqueue();
        WorkInfo last = await(second, 20);
        Log.i(TAG, "chain: first " + await(first, 5).getState() + ", second " + last.getState() + ", echo "
                + last.getOutputData().getString("echo"));
    }

    private void periodic() throws Exception {
        int before = CountWorker.RUNS.get();
        PeriodicWorkRequest request = new PeriodicWorkRequest.Builder(CountWorker.class, 15, TimeUnit.MINUTES)
                .setInputData(input(5)).build();
        mWork.enqueueUniquePeriodicWork("periodic", ExistingPeriodicWorkPolicy.KEEP, request);
        for (int i = 0; i < 100 && CountWorker.RUNS.get() == before; i++) {
            Thread.sleep(100);
        }
        Thread.sleep(500);
        Log.i(TAG, "periodic: first run " + (CountWorker.RUNS.get() - before) + ", state "
                + mWork.getWorkInfoById(request.getId()).get().getState());
        mWork.cancelUniqueWork("periodic");
        Log.i(TAG, "periodic, cancelled: " + await(request, 10).getState());
    }

    private void delayedCancel() throws Exception {
        OneTimeWorkRequest request = new OneTimeWorkRequest.Builder(CountWorker.class)
                .setInitialDelay(1, TimeUnit.HOURS).build();
        mWork.enqueue(request);
        Thread.sleep(500);
        Log.i(TAG, "delayed: " + mWork.getWorkInfoById(request.getId()).get().getState());
        mWork.cancelWorkById(request.getId());
        Log.i(TAG, "delayed, cancelled: " + await(request, 10).getState());
    }

    private void tags() throws Exception {
        OneTimeWorkRequest request = new OneTimeWorkRequest.Builder(CountWorker.class).addTag("tagged")
                .setInputData(input(6)).build();
        mWork.enqueue(request);
        await(request, 20);
        List<WorkInfo> infos = mWork.getWorkInfosByTag("tagged").get();
        Log.i(TAG, "tags: " + infos.size() + " tagged, " + infos.get(0).getState() + ", tags "
                + new java.util.TreeSet<String>(infos.get(0).getTags()).contains("tagged"));
    }

    private void expedited() throws Exception {
        OneTimeWorkRequest request = new OneTimeWorkRequest.Builder(CountWorker.class)
                .setExpedited(OutOfQuotaPolicy.RUN_AS_NON_EXPEDITED_WORK_REQUEST).setInputData(input(7)).build();
        mWork.enqueue(request);
        Log.i(TAG, "expedited: " + await(request, 20).getState());
    }
}
