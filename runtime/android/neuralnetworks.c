// libneuralnetworks.so's device count (the compatibility plan's gap 14): Minato has no NNAPI driver and no device, so a
// runtime that probes the library, as TensorFlow Lite's NNAPI delegate does, finds no device and runs on the CPU. The
// count is the only function that writes through its argument; the other 68 are generated stubs that answer
// ANEURALNETWORKS_OP_FAILED (stubgen.py).
#include <stddef.h>
#include <stdint.h>

#define ANEURALNETWORKS_NO_ERROR 0
#define ANEURALNETWORKS_UNEXPECTED_NULL 3

__attribute__((visibility("default"))) int ANeuralNetworks_getDeviceCount(uint32_t *numDevices) {
  if (numDevices == NULL) return ANEURALNETWORKS_UNEXPECTED_NULL;
  *numDevices = 0;
  return ANEURALNETWORKS_NO_ERROR;
}
