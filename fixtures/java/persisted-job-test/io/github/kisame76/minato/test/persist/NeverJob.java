package io.github.kisame76.minato.test.persist;

import android.app.job.JobParameters;
import android.app.job.JobService;

/** The job service of persisted-job-test: its jobs wait a minute, so it is not meant to run. */
public class NeverJob extends JobService {
    @Override
    public boolean onStartJob(JobParameters params) {
        return false;
    }

    @Override
    public boolean onStopJob(JobParameters params) {
        return false;
    }
}
