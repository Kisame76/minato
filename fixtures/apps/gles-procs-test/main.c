// gles-procs-test as an app: android_main runs graphics/tests/gles_procs_test.c, whose lines go to the log under the
// tag gles-procs-test, one entry per line as the test ends it, then calls ANativeActivity_finish and logs
// "android_main returns" at the end, which tools/java-start-check.sh waits for.
#include <android/log.h>
#include <android_native_app_glue.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const char *const tag = "gles-procs-test";

/* The line so far: the test prints a line in pieces. */
static char line[4096];
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
#define GLES_PROCS_MAIN gles_procs_main
#include "gles_procs_test.c"

void android_main(struct android_app *app) {
    gles_procs_main();
    ANativeActivity_finish(app->activity);
    while (!app->destroyRequested) {
        struct android_poll_source *source = NULL;
        if (ALooper_pollOnce(-1, NULL, NULL, (void **)&source) >= 0 && source != NULL) {
            source->process(app, source);
        }
    }
    __android_log_write(ANDROID_LOG_INFO, tag, "android_main returns");
}
