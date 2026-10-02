// libvndksupport's interface (system/core/libvndksupport/include/vndksupport/linker.h), the part libutils' misc.cpp
// calls, for our stand-in in framework/native/vndksupport (ADR-0039 b3.2).
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void* android_load_sphal_library(const char* name, int flag);
int android_unload_sphal_library(void* handle);

#ifdef __cplusplus
}
#endif
