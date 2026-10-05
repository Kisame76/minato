// Hidden members of the public API's compile-only stub (ADR-0039 step c2c2), which tools/managergen joins with it:
// View hands a drop's permissions on, and TextView's receive-content listener names the annotations (ADR-0043 step
// d)).
import android.view.inputmethod.InputContentInfo;

class ContentInfo {
    public static class Builder {
        /** @hide BaseInputConnection's, for content an input method commits (ADR-0043 step d)). */
        public Builder setInputContentInfo(InputContentInfo inputContentInfo) {
            throw new RuntimeException("stub");
        }

        /** @hide */
        public Builder setDragAndDropPermissions( DragAndDropPermissions permissions) {
            throw new RuntimeException("stub");
        }
    }

    /** @hide */
    public @interface Source {
    }

    /** @hide */
    public @interface Flags {
    }
}
