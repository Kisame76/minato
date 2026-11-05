package io.github.kisame76.minato.test.run;

import android.app.job.JobParameters;
import android.app.job.JobService;
import android.os.Handler;
import android.os.Looper;

/** The job service of run-test: records the start of a job, ends it after a moment, and records the stop. */
public class TestJob extends JobService {
    @Override
    public boolean onStartJob(JobParameters params) {
        RunApplication.jobStarted(params.getJobId(), params.isOverrideDeadlineExpired());
        new Handler(Looper.getMainLooper()).postDelayed(() -> {
            RunApplication.jobEnded(params.getJobId());
            jobFinished(params, false);
        }, 200);
        return true;
    }

    @Override
    public boolean onStopJob(JobParameters params) {
        RunApplication.jobStopped(params.getJobId());
        return false;
    }
}
