package io.github.kisame76.minato.test.jobs;

import android.app.job.JobParameters;
import android.app.job.JobService;

/** A job service that never gets to run in the test: its jobs wait for an hour. */
public class OpenJob extends JobService {
    @Override
    public boolean onStartJob(JobParameters params) {
        return false;
    }

    @Override
    public boolean onStopJob(JobParameters params) {
        return false;
    }
}
