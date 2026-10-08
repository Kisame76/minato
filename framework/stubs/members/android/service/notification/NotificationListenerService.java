// Hidden members of the public API's compile-only stub (ADR-0045 gap 1, batch 2), which tools/managergen joins with
// it: NotificationChannel's JSON form, the system's, names an importance.
class NotificationListenerService {
    public static class Ranking {
        /** @hide */
        public static String importanceToString(int importance) {
            throw new RuntimeException("stub");
        }
    }
}
