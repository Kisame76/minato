/*
 * Our aconfig flags of the job scheduler's framework (package android.app.job), whose declarations are not among the
 * fetched sources: the name that JobInfo's @FlaggedApi gives and the one flag JobInfo.enforceValidity reads.
 * enforce_minimum_time_windows has no value in build_release's ap3a set, so it has its declaration's default,
 * DISABLED [verified: apex/jobscheduler/framework/aconfig/job.aconfig at android-15.0.0_r1]; job_debug_info_apis is
 * ENABLED there, and only named.
 */
package android.app.job;

/** @hide */
public final class Flags {
    private Flags() {
    }

    public static final String FLAG_JOB_DEBUG_INFO_APIS = "android.app.job.job_debug_info_apis";

    public static boolean enforceMinimumTimeWindows() {
        return false;
    }
}
