package io.github.kisame76.minato.test.jobsalarms;

import android.app.job.JobParameters;
import android.app.job.JobService;
import android.app.job.JobWorkItem;
import android.os.Handler;
import android.os.Looper;

/** The jobs' service: each job id does what its test needs, and tells the test what its params say. */
public class TestJob extends JobService {
    @Override
    public boolean onStartJob(JobParameters params) {
        final int id = params.getJobId();
        JobsAlarmsApplication.record("job " + id + " started");
        switch (id) {
            case 1:
                JobsAlarmsApplication.fact("expedited job: expedited " + params.isExpeditedJob() + ", user-initiated "
                        + params.isUserInitiatedJob() + ", stop reason " + params.getStopReason() + ", deadline expired "
                        + params.isOverrideDeadlineExpired() + ", network " + (params.getNetwork() != null)
                        + ", extras " + params.getExtras().getString("key") + ", transient "
                        + params.getTransientExtras().getString("key") + ", job id " + id);
                break;
            case 2:
                JobsAlarmsApplication.fact("regular job: expedited " + params.isExpeditedJob() + ", stop reason "
                        + params.getStopReason() + ", triggered uris " + params.getTriggeredContentUris()
                        + ", clip data " + params.getClipData() + ", granted " + params.getClipGrantFlags());
                break;
            case 3:
                // The first run asks to be rescheduled by the backoff; the second finishes.
                final boolean again = JobsAlarmsApplication.count("job 3 started") == 1;
                final Handler handler3 = new Handler(Looper.getMainLooper());
                handler3.postDelayed(() -> jobFinished(params, again), 100);
                if (again) {
                    // Right after its first run asked for a reschedule, the job is pending again with its backoff.
                    handler3.postDelayed(() -> {
                        final android.app.job.JobInfo pending = getSystemService(android.app.job.JobScheduler.class)
                                .getPendingJob(3);
                        JobsAlarmsApplication.fact("job 3 right after its reschedule: pending " + (pending != null)
                                + ", backoff " + (pending == null ? null : pending.getInitialBackoffMillis() + " "
                                + pending.getBackoffPolicy()));
                    }, 400);
                }
                return true;
            case 4:
                return true;  // runs until cancelled
            case 5:
                final Handler handler = new Handler(Looper.getMainLooper());
                handler.post(() -> {
                    JobWorkItem item;
                    while ((item = params.dequeueWork()) != null) {
                        JobsAlarmsApplication.record("work " + item.getIntent().getAction() + " delivery "
                                + item.getDeliveryCount());
                        params.completeWork(item);
                    }
                    jobFinished(params, false);
                });
                return true;
            default:
                break;
        }
        if (id == 1 || id == 2) {
            new Handler(Looper.getMainLooper()).postDelayed(() -> jobFinished(params, false), 100);
            return true;
        }
        return false;
    }

    @Override
    public boolean onStopJob(JobParameters params) {
        JobsAlarmsApplication.record("job " + params.getJobId() + " stopped");
        JobsAlarmsApplication.fact("job " + params.getJobId() + " stop reason " + params.getStopReason());
        return false;
    }
}
