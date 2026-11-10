// The written part of PerformanceHintManager (services bundle A of the system services gap): the hints a game gives
// the system about its threads' work. Minato has no scheduler to tell, so the manager and its sessions are there and do
// nothing, as the emulator (apk-runner-35-bare) shows an app a manager and a session whose calls come back normally
// [verified: services-f-test]. The preferred update rate is the display's 60 Hz period; ArgumentChecks are AOSP's
// [inferred from android-15.0.0_r1, android/os/PerformanceHintManager.java].
import java.io.Closeable;

class PerformanceHintManager {
    /** @hide As the tag's, which SystemServiceRegistry calls. */
    public PerformanceHintManager() {
    }

    public long getPreferredUpdateRateNanos() {
        return 16_666_666L;
    }

    public Session createHintSession(int[] tids, long initialTargetWorkDurationNanos) {
        java.util.Objects.requireNonNull(tids);
        if (tids.length == 0) throw new IllegalArgumentException("thread id list can't be empty.");
        return new Session();
    }

    public static class Session implements Closeable {
        private boolean mClosed;

        /** @hide */
        Session() {
        }

        public void updateTargetWorkDuration(long targetDurationNanos) {
            if (targetDurationNanos <= 0) {
                throw new IllegalArgumentException("the hint's target duration should be positive.");
            }
        }

        public void reportActualWorkDuration(long actualDurationNanos) {
            if (actualDurationNanos <= 0) {
                throw new IllegalArgumentException("the actual duration should be positive.");
            }
        }

        public void setThreads(int[] tids) {
            java.util.Objects.requireNonNull(tids);
            if (tids.length == 0) throw new IllegalArgumentException("the list of selected threads is empty.");
        }

        public void setPreferPowerEfficiency(boolean enabled) {
        }

        public void close() {
            mClosed = true;
        }
    }
}
