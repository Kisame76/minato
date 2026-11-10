package io.github.kisame76.minato.test.workmanager;

import android.content.Context;
import android.os.Looper;
import android.util.Log;
import androidx.annotation.NonNull;
import androidx.work.Data;
import androidx.work.Worker;
import androidx.work.WorkerParameters;
import java.util.concurrent.atomic.AtomicInteger;

/** The test's workers: count and echo succeed, fail fails; each logs that it ran, and on which kind of thread. */
public class CountWorker extends Worker {
    static final AtomicInteger RUNS = new AtomicInteger();

    public CountWorker(@NonNull Context context, @NonNull WorkerParameters parameters) {
        super(context, parameters);
    }

    @NonNull
    @Override
    public Result doWork() {
        int n = getInputData().getInt("n", 0);
        RUNS.incrementAndGet();
        boolean main = Looper.myLooper() == Looper.getMainLooper();
        Log.i(WorkActivity.TAG, "worker count ran, n " + n + ", main thread " + main + ", attempt "
                + getRunAttemptCount());
        return Result.success(new Data.Builder().putString("out", "ran:" + n).build());
    }
}
