// The part of the NDK's <android/log.h> that runtime/android/aaudio.cpp uses, for its build on the Mac
// (aaudio_library_test.cpp writes the lines to stderr).
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

enum { ANDROID_LOG_DEBUG = 3, ANDROID_LOG_INFO = 4, ANDROID_LOG_WARN = 5, ANDROID_LOG_ERROR = 6 };

int __android_log_print(int priority, const char *tag, const char *format, ...);

#ifdef __cplusplus
}
#endif
