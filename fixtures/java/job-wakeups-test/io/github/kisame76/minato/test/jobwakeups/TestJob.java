package io.github.kisame76.minato.test.jobwakeups;

import android.app.job.JobParameters;
import android.app.job.JobService;

/** The job that is not due for an hour; it never runs in the test. */
public class TestJob extends JobService {
    @Override
    public boolean onStartJob(JobParameters params) {
        return false;
    }

    @Override
    public boolean onStopJob(JobParameters params) {
        return false;
    }
}
