// The written part of Toolbar (the Unity test APK retest 10: Activity's setActionBar names the type, and a class whose method types
// cannot be resolved cannot be read by reflection), which tools/managergen joins with the API's methods. AOSP's
// Toolbar needs the action menu views, which framework.jar lacks yet, so the class is an empty view group: its
// constructors are View's, so that a layout can inflate one, and it lays out no children.
import android.content.Context;
import android.util.AttributeSet;

class Toolbar {
    public Toolbar(Context context) {
        this(context, null);
    }

    public Toolbar(Context context, AttributeSet attrs) {
        this(context, attrs, com.android.internal.R.attr.toolbarStyle);
    }

    public Toolbar(Context context, AttributeSet attrs, int defStyleAttr) {
        this(context, attrs, defStyleAttr, 0);
    }

    public Toolbar(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes) {
        super(context, attrs, defStyleAttr, defStyleRes);
    }
}
