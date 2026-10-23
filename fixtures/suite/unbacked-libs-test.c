// unbacked-libs-test: the NDK libraries that Android 15 has and Minato has nothing behind (the compatibility plan's
// gap 14): libsync, libamidi, libbinder_ndk, libneuralnetworks and libOpenMAXAL. A program that names them in NEEDED
// must load, and each function it takes comes from its own library. What the libraries do differs from a phone's on
// purpose (Android's NNAPI lists devices, its OpenMAX AL makes an engine) and the generated stubs log, so only what
// does not differ is printed: the library of each name, the status of the device count and of the engine interface
// count and that each is written, and that two interface IDs differ in content. An interface ID is a pointer variable;
// on Android 15 it points into libwilhelm.so, so its own place is asked.
#define _GNU_SOURCE
#include <OMXAL/OpenMAXAL.h>
#include <amidi/AMidi.h>
#include <android/NeuralNetworks.h>
#include <android/binder_ibinder.h>
#include <android/sync.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// The file name of the library that holds `address`, without its directory.
static const char *library_of(const void *address) {
  Dl_info info;
  memset(&info, 0, sizeof(info));
  if (address == NULL || dladdr(address, &info) == 0 || info.dli_fname == NULL) return "(none)";
  const char *slash = strrchr(info.dli_fname, '/');
  return slash != NULL ? slash + 1 : info.dli_fname;
}

int main(void) {
  printf("sync_merge from: %s\n", library_of((const void *)sync_merge));
  printf("AMidiDevice_release from: %s\n", library_of((const void *)AMidiDevice_release));
  printf("AIBinder_new from: %s\n", library_of((const void *)AIBinder_new));
  printf("ANeuralNetworks_getDeviceCount from: %s\n", library_of((const void *)ANeuralNetworks_getDeviceCount));
  printf("xaCreateEngine from: %s\n", library_of((const void *)xaCreateEngine));
  printf("XA_IID_ENGINE from: %s\n", library_of((const void *)&XA_IID_ENGINE));

  uint32_t devices = 99;
  int status = ANeuralNetworks_getDeviceCount(&devices);
  printf("ANeuralNetworks_getDeviceCount: status %d, count %s\n", status, devices != 99 ? "written" : "untouched");
  XAuint32 interfaces = 99;
  status = (int)xaQueryNumSupportedEngineInterfaces(&interfaces);
  printf("xaQueryNumSupportedEngineInterfaces: status %d, count %s\n", status,
         interfaces != 99 ? "written" : "untouched");
  printf("XA_IID_ENGINE and XA_IID_PLAY: %s\n", memcmp(XA_IID_ENGINE, XA_IID_PLAY, sizeof(*XA_IID_ENGINE)) != 0 ? "differ" : "the same");
  return 0;
}
