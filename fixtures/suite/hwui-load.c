// hwui-load (ADR-0043 a1): opens libhwui.so as Android's VM start does through libandroid_runtime, finds the two
// functions libandroid_runtime calls there, and calls init_android_graphics, which sets up Skia for the process.
// graphics.hwui_load runs it as /system/bin/hwui-load, with libhwui.so and the libraries it needs in /system/lib64
// and ART's folder for ICU and libnativehelper, as on Android 15 (graphics/tests/hwui_load_run.py).
#include <dlfcn.h>
#include <stdio.h>

int main(void) {
    void *hwui = dlopen("libhwui.so", RTLD_NOW);
    if (hwui == NULL) {
        printf("dlopen(libhwui.so): %s\n", dlerror());
        return 1;
    }
    printf("dlopen(libhwui.so): a handle\n");
    void (*init)(void) = (void (*)(void))dlsym(hwui, "init_android_graphics");
    printf("init_android_graphics: %s\n", init != NULL ? "found" : "missing");
    printf("register_android_graphics_classes: %s\n",
           dlsym(hwui, "register_android_graphics_classes") != NULL ? "found" : "missing");
    if (init == NULL) {
        return 1;
    }
    init();
    printf("init_android_graphics(): returned\n");
    return 0;
}
