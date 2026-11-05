// The written part of ShareActionProvider (services batch C): the action bar's share button. AOSP's builds an
// ActivityChooserView from a model of the activities that take the intent; ours keeps the intent, the history file's
// name and the listener, offers the sub menu AOSP's does (the targets of the intent) and gives the action view an
// empty View, since Minato's action bar shows the sub menu [verified: services-c-test for the answers of hasSubMenu,
// onCreateActionView and setShareIntent].
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.view.MenuItem;
import android.view.SubMenu;
import android.view.View;

import java.util.List;

class ShareActionProvider {
    private static final int DEFAULT_INITIAL_ACTIVITY_COUNT = 4;

    private final android.content.Context mContext;
    private int mMaxShownActivityCount = DEFAULT_INITIAL_ACTIVITY_COUNT;
    private String mShareHistoryFileName = DEFAULT_SHARE_HISTORY_FILE_NAME;
    private Intent mIntent;
    private OnShareTargetSelectedListener mOnShareTargetSelectedListener;

    public ShareActionProvider(android.content.Context context) {
        super(context);
        mContext = context;
    }

    public void setOnShareTargetSelectedListener(OnShareTargetSelectedListener listener) {
        mOnShareTargetSelectedListener = listener;
    }

    public View onCreateActionView() {
        return new View(mContext);
    }

    /** @hide Overrides ActionProvider's. */
    public boolean hasSubMenu() {
        return true;
    }

    /** @hide Overrides ActionProvider's. */
    public void onPrepareSubMenu(SubMenu subMenu) {
        subMenu.clear();
        if (mIntent == null) {
            return;
        }
        final PackageManager packageManager = mContext.getPackageManager();
        final List<ResolveInfo> targets = packageManager.queryIntentActivities(mIntent, 0);
        for (int i = 0; i < targets.size() && i < mMaxShownActivityCount; i++) {
            final ResolveInfo target = targets.get(i);
            final MenuItem item = subMenu.add(0, i, i, target.loadLabel(packageManager));
            item.setIcon(target.loadIcon(packageManager));
            item.setOnMenuItemClickListener(clicked -> {
                final Intent intent = new Intent(mIntent);
                intent.setComponent(new android.content.ComponentName(target.activityInfo.packageName,
                        target.activityInfo.name));
                if (mOnShareTargetSelectedListener != null
                        && mOnShareTargetSelectedListener.onShareTargetSelected(this, intent)) {
                    return true;
                }
                mContext.startActivity(intent);
                return true;
            });
        }
    }

    public void setShareHistoryFileName(String shareHistoryFile) {
        mShareHistoryFileName = shareHistoryFile;
    }

    public void setShareIntent(Intent shareIntent) {
        if (shareIntent != null) {
            final String action = shareIntent.getAction();
            if (Intent.ACTION_SEND.equals(action) || Intent.ACTION_SEND_MULTIPLE.equals(action)) {
                shareIntent.addFlags(Intent.FLAG_ACTIVITY_NEW_DOCUMENT | Intent.FLAG_ACTIVITY_MULTIPLE_TASK);
            }
        }
        mIntent = shareIntent;
    }
}
