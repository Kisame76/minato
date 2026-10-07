// ADR-0044 c), test 2's last part on the Mac: graphics/host's run-time check before the ES 3.1 path passes on this
// GPU, saying how long it took (c2 measures that at the iPhone's session start), and refuses with a reason when
// forced to fail, as the App's Debug override forces it.
#include <stdio.h>

#include "minato_graphics.h"

int main(void) {
    int failures = 0;
    char reason[256] = "";
    double milliseconds = -1;
    if (minato_graphics_angle_vulkan_check(0, reason, sizeof reason) != 1) {
        printf("FAIL: the run-time check refuses the ES 3.1 path: %s\n", reason);
        failures++;
    } else if (sscanf(reason, "passed in %lf ms", &milliseconds) != 1 || milliseconds <= 0) {
        printf("FAIL: the run-time check passes without its time: \"%s\"\n", reason);
        failures++;
    } else {
        printf("angle vulkan check: %s\n", reason);
    }
    reason[0] = '\0';
    if (minato_graphics_angle_vulkan_check(1, reason, sizeof reason) != 0 || reason[0] == '\0') {
        printf("FAIL: the run-time check, forced to fail, does not refuse with a reason\n");
        failures++;
    } else {
        printf("angle vulkan check: forced to fail, it refuses: %s\n", reason);
    }
    return failures == 0 ? 0 : 1;
}
