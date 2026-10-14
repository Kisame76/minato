/* buffer-queue-host-test (graphics/tests/CMakeLists.txt): the NDK's android/ headers built for the Mac, whose
   sys/cdefs.h lacks bionic's availability macros; the API level is the guest's. */
#define __INTRODUCED_IN(api_level)
#define __DEPRECATED_IN(api_level, ...)
#define __REMOVED_IN(api_level, ...)
#define __ANDROID_API__ 35
