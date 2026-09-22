// C interface of the runtime (loader and Linux syscall layer) for the app.
// Plain C, so Swift can import it as the module APKRuntime (module.modulemap).
#ifndef APKRUNTIME_APKRT_H
#define APKRUNTIME_APKRT_H

#ifdef __cplusplus
extern "C" {
#endif

/// Version of the runtime, e.g. "0.1.0".
const char *apkrt_version(void);

#ifdef __cplusplus
}
#endif

#endif
