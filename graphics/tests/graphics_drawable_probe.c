/* graphics-drawable-probe: graphics/host's probe of ADR-0041 decision 11 c) on the Mac, for half a second per run, as
   a debug build of the app runs it on the iPhone for 3 s. Off screen and paced by a sleep, its numbers say nothing
   about the pool here; the test checks what it prints. */
#include "minato_graphics.h"

int main(void) {
    return minato_drawable_probe(0.5);
}
