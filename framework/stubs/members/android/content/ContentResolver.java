// Hidden members of the public API's compile-only stub (ADR-0039 step c1), which tools/managergen joins
// with it: FileUtils' MIME type helpers name the default type. javac inlines the constant, so it carries AOSP's value.
// (ADR-0039 step c2c1) ContentProvider's transport answers with them.
// (ADR-0039 step c2c1) IContentProvider's defaults pack a selection.
import android.os.Bundle;

class ContentResolver {
    /** @hide */
    public static final String MIME_TYPE_DEFAULT = "application/octet-stream";

    /** @hide */
    public static final String REMOTE_CALLBACK_RESULT = "result";

    /** @hide */
    public static final String REMOTE_CALLBACK_ERROR = "error";

    /** @hide */
    public static String createSqlSortClause(Bundle queryArgs) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public static Bundle createSqlQueryBundle(String selection, String[] selectionArgs) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public static Bundle createSqlQueryBundle(String selection, String[] selectionArgs, String sortOrder) {
        throw new RuntimeException("stub");
    }
}
