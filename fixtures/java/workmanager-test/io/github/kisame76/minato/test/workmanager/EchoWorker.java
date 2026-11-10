package io.github.kisame76.minato.test.workmanager;

import android.content.Context;
import android.util.Log;
import androidx.annotation.NonNull;
import androidx.work.Data;
import androidx.work.Worker;
import androidx.work.WorkerParameters;

/** The second link of the chain: it hands on what the first one produced. */
public class EchoWorker extends Worker {
    public EchoWorker(@NonNull Context context, @NonNull WorkerParameters parameters) {
        super(context, parameters);
    }

    @NonNull
    @Override
    public Result doWork() {
        String in = getInputData().getString("out");
        Log.i(WorkActivity.TAG, "worker echo ran, input out " + in);
        return Result.success(new Data.Builder().putString("echo", "echo:" + in).build());
    }
}
