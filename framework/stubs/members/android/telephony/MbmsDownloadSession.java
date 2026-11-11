// Hidden members of the public API's compile-only stub (framework breadth, batch H1): the annotations of the MBMS
// states and the actions and metadata names of the services, which the MBMS helpers name.
class MbmsDownloadSession {
    /** @hide */
    public @interface DownloadStatus {
    }

    /** @hide */
    public static final String MBMS_DOWNLOAD_SERVICE_ACTION = "android.telephony.action.EmbmsDownload";

    /** @hide */
    public static final String MBMS_DOWNLOAD_SERVICE_OVERRIDE_METADATA = "mbms-download-service-override";
}
