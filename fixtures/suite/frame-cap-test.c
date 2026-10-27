// frame-cap-test: an app on an engine that caps its own frame rate (ADR-0066, FPS unlock), through
// libframe-cap-engine.so and libframe-cap-api.so, which stand in for Unity and its scripting runtime by their names
// only. The engine registers its internal calls, then the test calls the setter as an app's script does and reads
// the rate back. Expected output: fixtures/expected/frame-cap-test.out
#include <stdio.h>

typedef void (*Method)(void);
Method frame_cap_internal_call(const char *name);
int frame_cap_engine_start(void);
int frame_cap_engine_rate(void);

int main(void) {
    if (frame_cap_engine_start() != 0) {
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
    set(60);
    printf("frame-cap-test: the app set 60, the getter answers %d, the engine runs at %d fps\n", get(),
           frame_cap_engine_rate());
    return 0;
}
