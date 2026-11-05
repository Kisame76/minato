/*
 * The job scheduler (ADR-0045: jobs and alarms kept, none run; gap 9 of the compatibility plan): IJobScheduler of
 * system_server, local in the app's process, which ServiceManager's table gives as "jobscheduler" and
 * android.app.JobSchedulerImpl asks. It answers as Android 15's JobSchedulerService does for its app [verified:
 * jobs-test on apk-runner-35-bare, and JobSchedulerService.JobSchedulerStub and scheduleAsPackage at the tag]:
 *   - a job is checked as the stub checks it: the job service is a service of the app that requires BIND_JOB_SERVICE,
 *     a persisted job needs RECEIVE_BOOT_COMPLETED, a job with a network needs ACCESS_NETWORK_STATE, JobInfo's own
 *     validity, the flags only the system sets, a work item's rules, and the namespace's;
 *   - the job is kept by namespace and ID, and the same ID replaces it; a work item joins the equal job's queue; an
 *     app may keep 151 jobs, and the next one is refused with Android's message;
 *   - the pending jobs, one, the reason, cancel by ID, in a namespace, and all are answered from what is kept.
 * The jobs live as long as the process, where Android keeps the persisted ones across boots (a persisted job could be
 * put back at the app's next start, which is not done yet), and they run while it lives (the compatibility plan's
 * decision 6): a daemon thread starts a job when its minimum latency has passed and its constraints hold, or its
 * deadline has passed [measured: run-test on apk-runner-35: a job without a constraint starts within milliseconds, one
 * with an unmet constraint at its deadline, with isOverrideDeadlineExpired], by binding the app's JobService and
 * calling startJob, and ends it when the service calls jobFinished (a rescheduled one waits for its backoff, a
 * periodic one for its next period) or when it is cancelled (stopJob). The constraints: the network from the
 * ConnectivityManager's active network, charging and a battery that is not low from the battery's sticky broadcast,
 * storage never low, the device never idle, content URIs never triggered. Nothing runs while iOS suspends the app or
 * after it ends; no Doze, standby buckets or quotas. A persisted job is not put back at the app's next start (a later
 * step). Not checked: the user-initiated job's permission and state, the rate limit of persisted jobs, the expedited
 * jobs' quota, and a service of another package (the package manager knows the app's own only). Every other
 * call is AIDL's Default answer. Not under com.android.minato, whose classes tools/framework-host-check.sh compiles on
 * the host without android.*.
 */
package android.app;

import android.Manifest;
import android.app.job.IJobCallback;
import android.app.job.IJobScheduler;
import android.app.job.IJobService;
import android.app.job.JobParameters;
import android.app.job.JobInfo;
import android.app.job.JobScheduler;
import android.app.job.JobService;
import android.app.job.JobWorkItem;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.ServiceConnection;
import android.content.pm.PackageManager;
import android.content.pm.PackageManager.NameNotFoundException;
import android.content.pm.ParceledListSlice;
import android.content.pm.ServiceInfo;
import android.os.Binder;
import android.os.Build;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.NetworkRequest;
import android.os.BatteryManager;
import android.os.IBinder;
import android.os.Process;
import android.os.RemoteException;
import android.os.SystemClock;
import android.util.ArrayMap;
import android.util.Log;

import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.Objects;

public final class LocalJobScheduler extends IJobScheduler.Default {
    /** JobSchedulerService's MAX_JOBS_PER_APP: the count before a new job is added must not exceed it. */
    private static final int MAX_JOBS_PER_APP = 150;

    /** A job kept: its namespace, the job, the work queued for it and when it was scheduled. */
    private static final class Entry {
        final String namespace;
        final JobInfo job;
        final List<JobWorkItem> work = new ArrayList<>();
        long scheduledAt = SystemClock.elapsedRealtime();
        /** The earliest start: the minimum latency (or a periodic job's window) and a backoff after a failure. */
        long notBefore;
        int failures;
        int lastWorkId;
        /** Set while the service runs the job. */
        boolean running;
        boolean deadlineExpired;
        JobParameters params;
        ServiceConnection connection;
        IBinder service;

        Entry(String namespace, JobInfo job) {
            this.namespace = namespace;
            this.job = job;
            notBefore = scheduledAt + (job.isPeriodic() ? Math.max(job.getIntervalMillis() - job.getFlexMillis(), 0)
                    : job.getMinLatencyMillis());
        }
    }

    private static final String TAG = "LocalJobScheduler";
    /** JobStatus's default backoff ceiling, five hours. */
    private static final long MAX_BACKOFF = 5 * 60 * 60 * 1000L;

    private final Binder mBinder = new Binder();
    /** The app's jobs, in every namespace; under the service's lock. */
    private final List<Entry> mEntries = new ArrayList<>();

    private LocalJobScheduler() {
        mBinder.attachInterface(this, IJobScheduler.Stub.DESCRIPTOR);
        final Thread runner = new Thread(this::runLoop, "JobScheduler");
        runner.setDaemon(true);
        runner.start();
    }

    /** The runner: starts the jobs that can run, looking again every quarter second and when a job is added. */
    private void runLoop() {
        while (true) {
            final List<Entry> candidates = new ArrayList<>();
            synchronized (mEntries) {
                final long now = SystemClock.elapsedRealtime();
                for (Entry entry : mEntries) {
                    if (!entry.running && now >= entry.notBefore) {
                        candidates.add(entry);
                    }
                }
                if (candidates.isEmpty()) {
                    try {
                        mEntries.wait(250);
                    } catch (InterruptedException e) {
                        return;
                    }
                    continue;
                }
            }
            boolean started = false;
            for (Entry entry : candidates) {
                final boolean met = constraintsMet(entry.job);
                final boolean expired = entry.job.hasLateConstraint() && SystemClock.elapsedRealtime()
                        >= entry.scheduledAt + (entry.job.isPeriodic() ? entry.job.getIntervalMillis()
                        : entry.job.getMaxExecutionDelayMillis());
                if (!met && !expired) {
                    continue;
                }
                synchronized (mEntries) {
                    if (entry.running || !mEntries.contains(entry)) {
                        continue;
                    }
                    entry.running = true;
                    entry.deadlineExpired = !met;
                }
                start(entry);
                started = true;
            }
            if (!started) {
                synchronized (mEntries) {
                    try {
                        mEntries.wait(250);
                    } catch (InterruptedException e) {
                        return;
                    }
                }
            }
        }
    }

    /** Whether the job's constraints hold now: the network, charging, battery, storage; never idle or a content URI. */
    private static boolean constraintsMet(JobInfo job) {
        final Application app = ActivityThread.currentApplication();
        if (app == null) {
            return false;
        }
        if (job.isRequireDeviceIdle() || job.getTriggerContentUris() != null) {
            return false;
        }
        if (job.isRequireCharging() || job.isRequireBatteryNotLow()) {
            final Intent battery = app.registerReceiver(null, new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
            if (battery == null) {
                return false;
            }
            if (job.isRequireCharging() && battery.getIntExtra(BatteryManager.EXTRA_PLUGGED, 0) == 0) {
                return false;
            }
            if (job.isRequireBatteryNotLow() && battery.getBooleanExtra(BatteryManager.EXTRA_BATTERY_LOW, false)) {
                return false;
            }
        }
        final NetworkRequest request = job.getRequiredNetwork();
        if (request != null) {
            final ConnectivityManager connectivity = app.getSystemService(ConnectivityManager.class);
            final Network network = connectivity != null ? connectivity.getActiveNetwork() : null;
            final NetworkCapabilities capabilities =
                    network != null ? connectivity.getNetworkCapabilities(network) : null;
            if (capabilities == null) {
                return false;
            }
            for (int capability : new int[] {NetworkCapabilities.NET_CAPABILITY_NOT_METERED,
                    NetworkCapabilities.NET_CAPABILITY_NOT_ROAMING}) {
                if (request.hasCapability(capability) && !capabilities.hasCapability(capability)) {
                    return false;
                }
            }
        }
        return true;
    }

    /** Binds the job's service and gives it the job (startJob, from onServiceConnected). */
    private void start(Entry entry) {
        final Application app = ActivityThread.currentApplication();
        final JobInfo job = entry.job;
        entry.params = new JobParameters(new Callback(entry), entry.namespace, job.getId(), job.getExtras(),
                job.getTransientExtras(), job.getClipData(), job.getClipGrantFlags(), entry.deadlineExpired,
                job.isExpedited(), job.isUserInitiated(), null, null, null);
        entry.connection = new ServiceConnection() {
            @Override
            public void onServiceConnected(ComponentName name, IBinder binder) {
                entry.service = binder;
                try {
                    IJobService.Stub.asInterface(binder).startJob(entry.params);
                } catch (RemoteException e) {
                    finish(entry, true);
                }
            }

            @Override
            public void onServiceDisconnected(ComponentName name) {
                finish(entry, true);
            }
        };
        if (app == null || !app.bindService(new Intent().setComponent(job.getService()), entry.connection,
                Context.BIND_AUTO_CREATE)) {
            Log.w(TAG, "could not bind " + job.getService());
            finish(entry, true);
        }
    }

    /** The job ended: unbound; kept for its backoff or its next period, or dropped. */
    private void finish(Entry entry, boolean reschedule) {
        final ServiceConnection connection;
        synchronized (mEntries) {
            if (!entry.running) {
                return;
            }
            entry.running = false;
            connection = entry.connection;
            entry.connection = null;
            entry.service = null;
            if (mEntries.contains(entry)) {
                final long now = SystemClock.elapsedRealtime();
                if (reschedule) {
                    entry.failures++;
                    final long initial = entry.job.getInitialBackoffMillis();
                    final long delay = entry.job.getBackoffPolicy() == JobInfo.BACKOFF_POLICY_LINEAR
                            ? initial * entry.failures : initial << Math.min(entry.failures - 1, 20);
                    entry.notBefore = now + Math.min(delay, MAX_BACKOFF);
                } else if (entry.job.isPeriodic()) {
                    entry.failures = 0;
                    entry.scheduledAt = now;
                    entry.notBefore = now + Math.max(entry.job.getIntervalMillis() - entry.job.getFlexMillis(), 0);
                } else {
                    mEntries.remove(entry);
                }
            }
            mEntries.notifyAll();
        }
        unbind(connection);
    }

    private static void unbind(ServiceConnection connection) {
        final Application app = ActivityThread.currentApplication();
        if (connection != null && app != null) {
            try {
                app.unbindService(connection);
            } catch (IllegalArgumentException e) {
                // already unbound
            }
        }
    }

    /** A job cancelled or replaced while it runs: stopJob, then unbound. */
    private void stopRunning(Entry entry) {
        final IBinder service;
        final ServiceConnection connection;
        final JobParameters params;
        synchronized (mEntries) {
            if (!entry.running) {
                return;
            }
            entry.running = false;
            service = entry.service;
            connection = entry.connection;
            params = entry.params;
            entry.connection = null;
            entry.service = null;
        }
        if (service != null) {
            try {
                IJobService.Stub.asInterface(service).stopJob(params);
            } catch (RemoteException e) {
                Log.w(TAG, "stopJob: " + e);
            }
        }
        unbind(connection);
    }

    /** The job's side of the call: what JobServiceEngine reports to the scheduler. */
    private final class Callback extends IJobCallback.Stub {
        private final Entry mEntry;
        private final List<Integer> mDelivered = new ArrayList<>();

        Callback(Entry entry) {
            mEntry = entry;
        }

        @Override
        public void acknowledgeStartMessage(int jobId, boolean ongoing) {
            if (!ongoing) {
                finish(mEntry, false);
            }
        }

        @Override
        public void acknowledgeStopMessage(int jobId, boolean reschedule) {
        }

        @Override
        public void acknowledgeGetTransferredDownloadBytesMessage(int jobId, int workId, long bytes) {
        }

        @Override
        public void acknowledgeGetTransferredUploadBytesMessage(int jobId, int workId, long bytes) {
        }

        @Override
        public JobWorkItem dequeueWork(int jobId) {
            synchronized (mEntries) {
                if (mEntry.work.isEmpty()) {
                    return null;
                }
                final JobWorkItem item = mEntry.work.remove(0);
                item.bumpDeliveryCount();
                item.setWorkId(++mEntry.lastWorkId);
                mDelivered.add(item.getWorkId());
                return item;
            }
        }

        @Override
        public boolean completeWork(int jobId, int workId) {
            synchronized (mEntries) {
                return mDelivered.remove(Integer.valueOf(workId));
            }
        }

        @Override
        public void jobFinished(int jobId, boolean reschedule) {
            finish(mEntry, reschedule);
        }

        @Override
        public void updateEstimatedNetworkBytes(int jobId, JobWorkItem item, long downloadBytes, long uploadBytes) {
        }

        @Override
        public void updateTransferredNetworkBytes(int jobId, JobWorkItem item, long downloadBytes, long uploadBytes) {
        }

        @Override
        public void setNotification(int jobId, int notificationId, Notification notification, int policy) {
        }
    }

    /** ServiceManager's maker for "jobscheduler": the Binder that carries a new service. */
    public static IBinder newBinder() {
        return new LocalJobScheduler().asBinder();
    }

    @Override
    public IBinder asBinder() {
        return mBinder;
    }

    @Override
    public int schedule(String namespace, JobInfo job) {
        enforceValidJobRequest(job);
        int result = validateJob(job, null);
        if (result != JobScheduler.RESULT_SUCCESS) {
            return result;
        }
        namespace = validateNamespace(namespace);
        job = enforceBuilderApiPermissions(job);
        return scheduleKept(job, null, namespace);
    }

    @Override
    public int enqueue(String namespace, JobInfo job, JobWorkItem work) {
        enforceValidJobRequest(job);
        if (work == null) {
            throw new NullPointerException("work is null");
        }
        int result = validateJob(job, work);
        if (result != JobScheduler.RESULT_SUCCESS) {
            return result;
        }
        namespace = validateNamespace(namespace);
        job = enforceBuilderApiPermissions(job);
        return scheduleKept(job, work, namespace);
    }

    @Override
    public int scheduleAsPackage(String namespace, JobInfo job, String packageName, int userId, String tag) {
        if (packageName == null) {
            throw new NullPointerException("Must specify a package for scheduleAsPackage()");
        }
        // The stub asks for UPDATE_DEVICE_STATS, which an app never holds.
        throw new SecurityException("Caller uid " + Process.myUid() + " not permitted to schedule jobs for other apps");
    }

    @Override
    public void cancel(String namespace, int jobId) {
        namespace = validateNamespace(namespace);
        Entry stopped = null;
        synchronized (mEntries) {
            Entry entry = find(namespace, jobId);
            if (entry != null) {
                mEntries.remove(entry);
                stopped = entry;
            }
        }
        if (stopped != null) {
            stopRunning(stopped);
        }
    }

    @Override
    public void cancelAll() {
        synchronized (mEntries) {
            mEntries.clear();
        }
    }

    @Override
    public void cancelAllInNamespace(String namespace) {
        namespace = validateNamespace(namespace);
        synchronized (mEntries) {
            for (int i = mEntries.size() - 1; i >= 0; i--) {
                if (Objects.equals(namespace, mEntries.get(i).namespace)) {
                    mEntries.remove(i);
                }
            }
        }
    }

    @Override
    public Map<String, ParceledListSlice<JobInfo>> getAllPendingJobs() {
        ArrayMap<String, List<JobInfo>> byNamespace = new ArrayMap<>();
        synchronized (mEntries) {
            for (int i = mEntries.size() - 1; i >= 0; i--) {
                Entry entry = mEntries.get(i);
                List<JobInfo> jobs = byNamespace.get(entry.namespace);
                if (jobs == null) {
                    jobs = new ArrayList<>();
                    byNamespace.put(entry.namespace, jobs);
                }
                jobs.add(entry.job);
            }
        }
        ArrayMap<String, ParceledListSlice<JobInfo>> out = new ArrayMap<>();
        for (int i = 0; i < byNamespace.size(); i++) {
            out.put(byNamespace.keyAt(i), new ParceledListSlice<>(byNamespace.valueAt(i)));
        }
        return out;
    }

    @Override
    public ParceledListSlice<JobInfo> getAllPendingJobsInNamespace(String namespace) {
        namespace = validateNamespace(namespace);
        List<JobInfo> jobs = new ArrayList<>();
        synchronized (mEntries) {
            for (int i = mEntries.size() - 1; i >= 0; i--) {
                if (Objects.equals(namespace, mEntries.get(i).namespace)) {
                    jobs.add(mEntries.get(i).job);
                }
            }
        }
        return new ParceledListSlice<>(jobs);
    }

    @Override
    public JobInfo getPendingJob(String namespace, int jobId) {
        namespace = validateNamespace(namespace);
        synchronized (mEntries) {
            Entry entry = find(namespace, jobId);
            return entry == null ? null : entry.job;
        }
    }

    @Override
    public int getPendingJobReason(String namespace, int jobId) {
        namespace = validateNamespace(namespace);
        Entry entry;
        synchronized (mEntries) {
            entry = find(namespace, jobId);
        }
        if (entry == null) {
            return JobScheduler.PENDING_JOB_REASON_INVALID_JOB_ID;
        }
        // JobStatus.getPendingJobReason's order for the constraints the job asks for, all unmet.
        JobInfo job = entry.job;
        if (job.isRequireBatteryNotLow()) {
            return JobScheduler.PENDING_JOB_REASON_CONSTRAINT_BATTERY_NOT_LOW;
        }
        if (job.isRequireCharging()) {
            return JobScheduler.PENDING_JOB_REASON_CONSTRAINT_CHARGING;
        }
        if (job.getRequiredNetwork() != null) {
            return JobScheduler.PENDING_JOB_REASON_CONSTRAINT_CONNECTIVITY;
        }
        if (job.getTriggerContentUris() != null && job.getTriggerContentUris().length > 0) {
            return JobScheduler.PENDING_JOB_REASON_CONSTRAINT_CONTENT_TRIGGER;
        }
        if (job.isRequireDeviceIdle()) {
            return JobScheduler.PENDING_JOB_REASON_CONSTRAINT_DEVICE_IDLE;
        }
        if (job.isPrefetch()) {
            return JobScheduler.PENDING_JOB_REASON_CONSTRAINT_PREFETCH;
        }
        if (job.isRequireStorageNotLow()) {
            return JobScheduler.PENDING_JOB_REASON_CONSTRAINT_STORAGE_NOT_LOW;
        }
        // A periodic job waits for the start of its first window, a one-off job for its minimum latency.
        long delay = job.isPeriodic() ? job.getIntervalMillis() - job.getFlexMillis() : job.getMinLatencyMillis();
        if (delay > 0 && SystemClock.elapsedRealtime() - entry.scheduledAt < delay) {
            return JobScheduler.PENDING_JOB_REASON_CONSTRAINT_MINIMUM_LATENCY;
        }
        return JobScheduler.PENDING_JOB_REASON_UNDEFINED;
    }

    @Override
    public boolean canRunUserInitiatedJobs(String packageName) {
        if (!Objects.equals(packageName, ActivityThread.currentPackageName())) {
            throw new SecurityException("Uid " + Process.myUid() + " cannot query canRunUserInitiatedJobs for package "
                    + packageName);
        }
        return hasPermission(Manifest.permission.RUN_USER_INITIATED_JOBS);
    }

    @Override
    public boolean hasRunUserInitiatedJobsPermission(String packageName, int userId) {
        if (!Objects.equals(packageName, ActivityThread.currentPackageName())) {
            throw new SecurityException("Uid " + Process.myUid()
                    + " cannot query hasRunUserInitiatedJobsPermission for package " + packageName);
        }
        return hasPermission(Manifest.permission.RUN_USER_INITIATED_JOBS);
    }

    /** JobSchedulerService.scheduleAsPackage for the app's own job. */
    private int scheduleKept(JobInfo job, JobWorkItem work, String namespace) {
        Entry replaced = null;
        synchronized (mEntries) {
            Entry existing = find(namespace, job.getId());
            if (work != null && existing != null && existing.job.equals(job)) {
                existing.work.add(work);
                mEntries.notifyAll();
                return JobScheduler.RESULT_SUCCESS;
            }
            if (mEntries.size() > MAX_JOBS_PER_APP) {
                throw new IllegalStateException("Apps may not schedule more than " + MAX_JOBS_PER_APP
                        + " distinct jobs");
            }
            Entry entry = new Entry(namespace, job);
            if (work != null) {
                entry.work.add(work);
            }
            if (existing != null) {
                mEntries.remove(existing);
                replaced = existing;
            }
            mEntries.add(entry);
            mEntries.notifyAll();
        }
        if (replaced != null) {
            stopRunning(replaced);
        }
        return JobScheduler.RESULT_SUCCESS;
    }

    private Entry find(String namespace, int jobId) {
        for (int i = mEntries.size() - 1; i >= 0; i--) {
            Entry entry = mEntries.get(i);
            if (entry.job.getId() == jobId && Objects.equals(namespace, entry.namespace)) {
                return entry;
            }
        }
        return null;
    }

    /** JobSchedulerStub.enforceValidJobRequest: the service is the app's own and requires BIND_JOB_SERVICE. */
    private static void enforceValidJobRequest(JobInfo job) {
        ComponentName service = job.getService();
        Application app = ActivityThread.currentApplication();
        try {
            ServiceInfo info = app.getPackageManager().getServiceInfo(service,
                    PackageManager.MATCH_DIRECT_BOOT_AWARE | PackageManager.MATCH_DIRECT_BOOT_UNAWARE);
            if (info == null) {
                throw new IllegalArgumentException("No such service " + service);
            }
            if (!JobService.PERMISSION_BIND.equals(info.permission)) {
                throw new IllegalArgumentException("Scheduled service " + service
                        + " does not require android.permission.BIND_JOB_SERVICE permission");
            }
        } catch (NameNotFoundException e) {
            throw new IllegalArgumentException("Tried to schedule job for non-existent component: " + service);
        }
        if (job.isPersisted() && !hasPermission(Manifest.permission.RECEIVE_BOOT_COMPLETED)) {
            throw new IllegalArgumentException("Requested job cannot be persisted without"
                    + " holding android.permission.RECEIVE_BOOT_COMPLETED permission");
        }
        if (job.getRequiredNetwork() != null && enabledAfter(Build.VERSION_CODES.TIRAMISU)
                && !hasPermission(Manifest.permission.ACCESS_NETWORK_STATE)) {
            throw new SecurityException(Manifest.permission.ACCESS_NETWORK_STATE
                    + " required for jobs with a connectivity constraint");
        }
    }

    /** JobSchedulerStub.validateJob, without the user-initiated job's permission and state. */
    @SuppressWarnings("deprecation") // CONNECTIVITY_INTERNAL, which the stub asks for a foreground-bound job.
    private static int validateJob(JobInfo job, JobWorkItem work) {
        boolean rejectNegativeNetworkEstimates = enabledAfter(Build.VERSION_CODES.TIRAMISU);
        job.enforceValidity(enabledSince(Build.VERSION_CODES.TIRAMISU), rejectNegativeNetworkEstimates,
                enabledAfter(Build.VERSION_CODES.UPSIDE_DOWN_CAKE), enabledAfter(Build.VERSION_CODES.UPSIDE_DOWN_CAKE));
        if ((job.getFlags() & JobInfo.FLAG_WILL_BE_FOREGROUND) != 0) {
            ActivityThread.currentApplication().enforceCallingOrSelfPermission(
                    Manifest.permission.CONNECTIVITY_INTERNAL, "JobScheduler");
        }
        if ((job.getFlags() & JobInfo.FLAG_EXEMPT_FROM_APP_STANDBY) != 0) {
            throw new SecurityException("Job has invalid flags");
        }
        if (work != null) {
            work.enforceValidity(rejectNegativeNetworkEstimates);
            if (work.getEstimatedNetworkDownloadBytes() != JobInfo.NETWORK_BYTES_UNKNOWN
                    || work.getEstimatedNetworkUploadBytes() != JobInfo.NETWORK_BYTES_UNKNOWN
                    || work.getMinimumNetworkChunkBytes() != JobInfo.NETWORK_BYTES_UNKNOWN) {
                if (job.getRequiredNetwork() == null && enabledAfter(Build.VERSION_CODES.TIRAMISU)) {
                    throw new IllegalArgumentException("JobWorkItem implies network usage"
                            + " but job doesn't specify a network constraint");
                }
            }
            if (job.isPersisted() && work.getIntent() != null) {
                throw new IllegalArgumentException("Cannot persist JobWorkItems with Intents");
            }
        }
        return JobScheduler.RESULT_SUCCESS;
    }

    /** JobSchedulerStub.validateNamespace: the sanitized namespace, or the exception. */
    private static String validateNamespace(String namespace) {
        namespace = JobScheduler.sanitizeNamespace(namespace);
        if (namespace != null) {
            if (namespace.isEmpty()) {
                throw new IllegalArgumentException("namespace cannot be empty");
            }
            if (namespace.length() > 1000) {
                throw new IllegalArgumentException("namespace cannot be more than 1000 characters");
            }
            namespace = namespace.intern();
        }
        return namespace;
    }

    /** JobSchedulerStub.enforceBuilderApiPermissions: an app may not set a bias. */
    private static JobInfo enforceBuilderApiPermissions(JobInfo job) {
        if (job.getBias() != JobInfo.BIAS_DEFAULT && !hasPermission(Manifest.permission.UPDATE_DEVICE_STATS)) {
            if (enabledAfter(Build.VERSION_CODES.UPSIDE_DOWN_CAKE)) {
                throw new SecurityException("Apps may not call setBias()");
            }
            return new JobInfo.Builder(job).setBias(JobInfo.BIAS_DEFAULT).build(false, false, false, false);
        }
        return job;
    }

    private static boolean hasPermission(String permission) {
        Application app = ActivityThread.currentApplication();
        return app != null && app.checkSelfPermission(permission) == PackageManager.PERMISSION_GRANTED;
    }

    /** CompatChanges for a change that is @EnabledAfter(sdk): the app targets a later SDK. */
    private static boolean enabledAfter(int sdk) {
        return targetSdk() > sdk;
    }

    /** CompatChanges for a change that is @EnabledSince(sdk): the app targets the SDK or a later one. */
    private static boolean enabledSince(int sdk) {
        return targetSdk() >= sdk;
    }

    private static int targetSdk() {
        Application app = ActivityThread.currentApplication();
        return app == null ? Build.VERSION.SDK_INT : app.getApplicationInfo().targetSdkVersion;
    }
}
