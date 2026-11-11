/* graphics-background-commit-probe: graphics/host's probe of Metal in the background (ADR-0029, decision 15) on the
   Mac, its three steps one after another, as a debug build of the app runs them across a trip to the background on
   the iPhone. Nothing restricts the GPU here, so the counter-case completes; the test checks what it prints. */
#include "minato_graphics.h"

int main(void) {
    if (minato_background_commit_probe_begin() != 0) {
        return 1;
    }
    minato_background_commit_probe_background(0.0);
    return minato_background_commit_probe_end();
}
