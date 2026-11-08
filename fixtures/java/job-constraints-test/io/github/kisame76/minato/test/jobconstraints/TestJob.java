package io.github.kisame76.minato.test.jobconstraints;

import android.app.job.JobParameters;
import android.app.job.JobService;

/**
 * The jobs run until the battery takes a constraint away; job 1 and job 2 ask to be rescheduled, job 3 does not; jobs 4
 * to 8 end at once.
 */
public class TestJob extends JobService {
    @Override
    public boolean onStartJob(JobParameters params) {
        ConstraintsApplication.event(params.getJobId(), "started");
        return params.getJobId() <= 3;
    }

    @Override
    public boolean onStopJob(JobParameters params) {
        ConstraintsApplication.event(params.getJobId(), "stopped " + params.getStopReason());
        return params.getJobId() != 3;
    }
}
