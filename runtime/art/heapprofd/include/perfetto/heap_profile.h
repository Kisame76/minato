// The part of heapprofd's client API (Perfetto's heap_profile.h) that ART calls (ADR-0033 a), as Android 15's
// heapprofd_client_api.so exports it. The guest's library is a stand-in: runtime/art/heapprofd.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AHeapInfo AHeapInfo;
typedef struct AHeapProfileEnableCallbackInfo AHeapProfileEnableCallbackInfo;
typedef struct AHeapProfileDisableCallbackInfo AHeapProfileDisableCallbackInfo;

uint64_t AHeapProfileEnableCallbackInfo_getSamplingInterval(const AHeapProfileEnableCallbackInfo* session_info);
AHeapInfo* AHeapInfo_create(const char* heap_name);
AHeapInfo* AHeapInfo_setEnabledCallback(
    AHeapInfo* info, void (*callback)(void* data, const AHeapProfileEnableCallbackInfo* session_info), void* data);
AHeapInfo* AHeapInfo_setDisabledCallback(
    AHeapInfo* info, void (*callback)(void* data, const AHeapProfileDisableCallbackInfo* session_info), void* data);
uint32_t AHeapProfile_registerHeap(AHeapInfo* info);
bool AHeapProfile_reportAllocation(uint32_t heap_id, uint64_t alloc_id, uint64_t size);
bool AHeapProfile_reportSample(uint32_t heap_id, uint64_t alloc_id, uint64_t size);
void AHeapProfile_reportFree(uint32_t heap_id, uint64_t alloc_id);

#ifdef __cplusplus
}
#endif
