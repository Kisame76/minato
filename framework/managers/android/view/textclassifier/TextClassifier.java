// The written part of TextClassifier (App, the Unity test APK retest 11: a class apps reflect over names it, so it ships as the API
// with default answers), which tools/managergen joins with the API's methods. Before: the hidden members of the public
// API's compile-only stub (the framework breadth's batch 1, the compatibility plan's entry 22), which tools/managergen
// joins with it: ClipDescription names the annotation of its entity types.
class TextClassifier {
    /** @hide */
    @interface EntityType {
    }

    /** @hide The widget types SelectionEvent names; the constants are the API's. */
    @interface WidgetType {
    }

    /** @hide What TextSelection's builder calls, as AOSP's TextClassifier.Utils has it. */
    final class Utils {
        static void checkArgument(CharSequence text, int startIndex, int endIndex) {
            com.android.internal.util.Preconditions.checkArgument(text != null);
            com.android.internal.util.Preconditions.checkArgument(startIndex >= 0);
            com.android.internal.util.Preconditions.checkArgument(endIndex <= text.length());
            com.android.internal.util.Preconditions.checkArgument(endIndex > startIndex);
        }
    }
}
