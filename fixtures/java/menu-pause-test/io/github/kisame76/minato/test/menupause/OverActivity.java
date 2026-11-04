package io.github.kisame76.minato.test.menupause;

import android.os.Bundle;
import android.widget.TextView;

/** The dialog themed activity of another task that comes over MainActivity; Back finishes it. */
public class OverActivity extends LoggedActivity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        TextView view = new TextView(this);
        view.setText("over");
        view.setPadding(80, 80, 80, 80);
        setContentView(view);
    }
}
