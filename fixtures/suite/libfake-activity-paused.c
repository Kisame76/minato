// libfake-activity-paused.so: stands in for the runtime's minato_activity_paused (1 from the activity's onPause to its
// onResume), which the SDL swap override asks (runtime/android/sdl.c), with a value the test sets. On Android nothing
// asks it.
static int paused;

void fake_activity_set_paused(int value) {
    paused = value;
}

int minato_activity_paused(void) {
    return paused;
}
