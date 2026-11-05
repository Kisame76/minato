package io.github.kisame76.minato.test.orientationrequest;

/** The activity without android:configChanges: Android relaunches it when it asks for another orientation. */
public class PlainActivity extends RequestActivity {
    @Override
    Class<?> next() {
        return ConfigActivity.class;
    }
}
