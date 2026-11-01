package io.github.kisame76.minato.test.back;

import android.app.Fragment;
import android.app.FragmentManager;
import android.os.Bundle;
import android.window.BackEvent;
import android.window.OnBackAnimationCallback;
import android.window.OnBackInvokedDispatcher;

/**
 * Started by Root, with fragment A and then fragment B on the back stack. In back-test, onBackPressed pops B and the
 * next BACK finishes the activity, as the system answers for an activity that is not the task's root. In
 * back-test-callback an OnBackAnimationCallback takes the first BACK, logs its calls, pops B and goes; the next BACK
 * reaches the activity's default callback.
 */
@SuppressWarnings("deprecation")
public class SecondActivity extends BackActivity {
    private final OnBackAnimationCallback mCallback = new OnBackAnimationCallback() {
        @Override
        public void onBackStarted(BackEvent event) {
            log("callback onBackStarted edge " + event.getSwipeEdge() + " progress " + event.getProgress());
        }

        @Override
        public void onBackProgressed(BackEvent event) {
            log("callback onBackProgressed progress " + event.getProgress());
        }

        @Override
        public void onBackCancelled() {
            log("callback onBackCancelled");
        }

        @Override
        public void onBackInvoked() {
            log("callback onBackInvoked");
            getOnBackInvokedDispatcher().unregisterOnBackInvokedCallback(this);
            getFragmentManager().popBackStack();
        }
    };

    /** A fragment without a view that logs its creation and its end. */
    public static class Named extends Fragment {
        @Override
        public void onCreate(Bundle savedInstanceState) {
            super.onCreate(savedInstanceState);
            ((BackActivity) getActivity()).log("fragment " + getTag() + " onCreate");
        }

        @Override
        public void onDestroy() {
            ((BackActivity) getActivity()).log("fragment " + getTag() + " onDestroy");
            super.onDestroy();
        }
    }

    @Override
    String name() {
        return "second";
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        FragmentManager fragments = getFragmentManager();
        fragments.addOnBackStackChangedListener(() -> log("back stack " + fragments.getBackStackEntryCount()));
        fragments.beginTransaction().add(new Named(), "A").commitNow();
        fragments.beginTransaction().add(new Named(), "B").addToBackStack("B").commit();
        if (callbacks()) {
            getOnBackInvokedDispatcher().registerOnBackInvokedCallback(OnBackInvokedDispatcher.PRIORITY_DEFAULT,
                    mCallback);
        }
    }
}
