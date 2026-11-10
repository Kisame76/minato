package io.github.kisame76.minato.test.workmanager;

import android.content.Context;
import android.util.Log;
import androidx.annotation.NonNull;
import androidx.work.Worker;
import androidx.work.WorkerParameters;

/** A worker that fails. */
public class FailWorker extends Worker {
    public FailWorker(@NonNull Context context, @NonNull WorkerParameters parameters) {
        super(context, parameters);
    }

    @NonNull
    @Override
    public Result doWork() {
        Log.i(WorkActivity.TAG, "worker fail ran");
        return Result.failure();
    }
}
