// Hidden members of the public API's compile-only stub (the Unity test APK probe's gap sort, category 1), which
// tools/managergen joins with it: what a PrintJob asks its print manager, and no Context gives a PrintManager yet.
class PrintManager {
    /** @hide */
    public PrintJobInfo getPrintJobInfo(PrintJobId printJobId) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public void cancelPrintJob(PrintJobId printJobId) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public void restartPrintJob(PrintJobId printJobId) {
        throw new RuntimeException("stub");
    }
}
