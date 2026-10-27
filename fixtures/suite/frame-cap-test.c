// frame-cap-test: an app on an engine that caps its own frame rate (ADR-0066, FPS unlock), through
// libframe-cap-engine.so and libframe-cap-api.so, which stand in for Unity and its scripting runtime by their names
// only. The engine registers its internal calls (with the argument "swap" the setter through the other registration),
// then the test calls the setter as an app's script does, with 60, -1 (Unity's default) and 240, and reads the rate
// back. Expected output: fixtures/expected/frame-cap-test.out, with either argument.
#include <stdio.h>
#include <string.h>

typedef void (*Method)(void);
Method frame_cap_internal_call(const char *name);
int frame_cap_engine_start(int swapped);
int frame_cap_engine_rate(void);

int main(int argc, char **argv) {
    if (frame_cap_engine_start(argc > 1 && strcmp(argv[1], "swap") == 0) != 0) {
        printf("frame-cap-test: the engine did not start\n");
        return 1;
    }
    printf("frame-cap-test: %d fps before the app sets a rate\n", frame_cap_engine_rate());
    void (*set)(int) = (void (*)(int))frame_cap_internal_call(
        "UnityEngine.Application::set_targetFrameRate(System.Int32)");
    int (*get)(void) = (int (*)(void))frame_cap_internal_call("UnityEngine.Application::get_targetFrameRate()");
    if (set == NULL || get == NULL) {
        printf("frame-cap-test: an internal call is missing\n");
        return 1;
    }
    const int rates[] = {60, -1, 240};
    for (int i = 0; i < 3; ++i) {
        set(rates[i]);
        printf("frame-cap-test: the app set %d, the getter answers %d, the engine runs at %d fps\n", rates[i], get(),
               frame_cap_engine_rate());
    }
    return 0;
}
