/* graphics-probe: graphics/host's probe (ADR-0030 V3) on the Mac, with the MoltenVK named on the
   command line, as a debug build of the app runs it on the iPhone. */
#include <stdio.h>

#include "minato_graphics.h"

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: graphics-probe <libMoltenVK.dylib>\n");
        return 2;
    }
    return minato_graphics_probe(argv[1]);
}
