/* graphics-provoking-probe: graphics/host's probe of ADR-0044 a) on the Mac, as a debug build of the app runs it on
   the iPhone: whether the last vertex provokes in a Metal pass set up as MoltenVK sets it up. */
#include "minato_graphics.h"

int main(void) {
    return minato_provoking_probe();
}
