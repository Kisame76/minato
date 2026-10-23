/* graphics.providers: minato_graphics_set_providers says which of its parts failed when it answers -1, since the
   caller gets only the -1. The runtime keeps four providers of "scheduled" (apkrt_add_scheduled_provider); with
   all four taken, MoltenVK's, ANGLE's, the windows' buffers' and the video decoders' providers find no room, and each
   says so once on stderr. Exits 1 when a line is missing or comes twice. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "APKRuntime/apkrt.h"
#include "minato_graphics.h"

static int scheduled(void *context, uint64_t deadline) {
    (void)context;
    (void)deadline;
    return 1;
}

/* How often `line` is in `text`. */
static int count(const char *text, const char *line) {
    int found = 0;
    for (const char *at = strstr(text, line); at != NULL; at = strstr(at + 1, line)) {
        found++;
    }
    return found;
}

int main(void) {
    for (int index = 0; index < 4; index++) {
        if (apkrt_add_scheduled_provider(scheduled, NULL) != 0) {
            fprintf(stderr, "FAILED  the runtime keeps fewer than four providers\n");
            return 1;
        }
    }
    char path[] = "/tmp/providers-test-XXXXXX";
    const int file = mkstemp(path);
    const int saved = dup(STDERR_FILENO);
    if (file < 0 || saved < 0) {
        fprintf(stderr, "FAILED  no file for stderr\n");
        return 1;
    }
    fflush(stderr);
    dup2(file, STDERR_FILENO);
    const int result = minato_graphics_set_providers("/nonexistent/libMoltenVK.dylib", "/nonexistent/libEGL.dylib");
    fflush(stderr);
    dup2(saved, STDERR_FILENO);
    char text[4096] = {0};
    const ssize_t length = pread(file, text, sizeof text - 1, 0);
    close(file);
    unlink(path);
    int failed = 0;
    if (result != -1) {
        fprintf(stderr, "FAILED  set_providers answered %d, not -1\n", result);
        failed = 1;
    }
    const char *const lines[] = {
        "minato graphics: no room for MoltenVK's provider of \"scheduled\"",
        "minato graphics: no room for ANGLE's provider of \"scheduled\"",
        "minato graphics: the windows' buffers were not set",
        "minato graphics: the video decoders were not set",
    };
    for (size_t index = 0; index < sizeof lines / sizeof *lines; index++) {
        if (count(length > 0 ? text : "", lines[index]) != 1) {
            fprintf(stderr, "FAILED  not once on stderr: %s\n", lines[index]);
            failed = 1;
        }
    }
    printf("%s", length > 0 ? text : "");
    return failed;
}
