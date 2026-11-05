// The written part of PrintManager (services batch C): printing, of which Minato has no spooler and no print service.
// print() answers as AOSP's does up to the spooler: only an activity may print, and a name and an adapter are needed
// [verified: services-c-test for the activity rule]; then there is no print job to return. What the print jobs ask of
// their manager has no job to ask about.
import android.content.ComponentName;
import android.content.Context;

import java.util.ArrayList;
import java.util.List;

class PrintManager {
    private static final String LOG_TAG = "PrintManager";

    private final Context mContext;

    /** @hide As the tag's, which SystemServiceRegistry calls with the outer context. */
    public PrintManager(Context context) {
        mContext = context;
    }

    public PrintJob print(String printJobName, PrintDocumentAdapter documentAdapter, PrintAttributes attributes) {
        if (!(mContext instanceof android.app.Activity)) {
            throw new IllegalStateException("Can print only from an activity");
        }
        if (printJobName == null || printJobName.isEmpty()) {
            throw new IllegalArgumentException("printJobName cannot be empty");
        }
        if (documentAdapter == null) {
            throw new IllegalArgumentException("documentAdapter cannot be null");
        }
        android.util.Log.w(LOG_TAG, "No print service: " + printJobName + " is not printed");
        return null;
    }

    public List<PrintJob> getPrintJobs() {
        return new ArrayList<>();
    }

    public boolean isPrintServiceEnabled(ComponentName service) {
        return false;
    }

    /** @hide */
    public PrintJobInfo getPrintJobInfo(PrintJobId printJobId) {
        return null;
    }

    /** @hide */
    public void cancelPrintJob(PrintJobId printJobId) {
    }

    /** @hide */
    public void restartPrintJob(PrintJobId printJobId) {
    }
}
