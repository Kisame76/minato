// vk-ahb-test as an app (ADR-0058 e1), decision 7): android_main runs graphics/tests/vk_ahb_test.c, whose lines go to
// the log under the tag vk-ahb-test, one entry per line as the test ends it, then calls ANativeActivity_finish and
// logs "android_main returns" at the end, which tools/android-reference.sh waits for. A test that fails ends the
// process at once, as the shell program does.
#include <android/log.h>
#include <android_native_app_glue.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const char *const tag = "vk-ahb-test";

/* The line so far: the test prints a line in pieces. */
static char line[512];
static size_t length;

static int say(const char *format, ...) {
    char piece[512];
    va_list arguments;
    va_start(arguments, format);
    const int written = vsnprintf(piece, sizeof piece, format, arguments);
    va_end(arguments);
    for (const char *at = piece; *at != '\0'; at++) {
        if (*at == '\n') {
            line[length] = '\0';
            __android_log_write(ANDROID_LOG_INFO, tag, line);
            length = 0;
        } else if (length + 1 < sizeof line) {
            line[length++] = *at;
        }
    }
    return written;
}

#define SAY say
#define VK_AHB_MAIN vk_ahb_main
#include "vk_ahb_test.c"

void android_main(struct android_app *app) {
    vk_ahb_main();
    ANativeActivity_finish(app->activity);
    while (!app->destroyRequested) {
        struct android_poll_source *source = NULL;
        if (ALooper_pollOnce(-1, NULL, NULL, (void **)&source) >= 0 && source != NULL) {
            source->process(app, source);
        }
    }
    __android_log_write(ANDROID_LOG_INFO, tag, "android_main returns");
}
