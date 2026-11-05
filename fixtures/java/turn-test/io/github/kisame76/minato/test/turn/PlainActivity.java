package io.github.kisame76.minato.test.turn;

/** The activity without android:configChanges: Android relaunches it at each turn. */
public class PlainActivity extends TurnActivity {
    @Override
    Class<?> next() {
        return ConfigActivity.class;
    }
}
