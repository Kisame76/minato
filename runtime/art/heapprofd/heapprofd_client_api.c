// heapprofd_client_api.so for the guest (ADR-0033 a): ART registers its Java heap at start and samples it once
// heapprofd enables it. heapprofd never runs here, so no heap is ever enabled, no callback is called and no
// report is taken.
#include "perfetto/heap_profile.h"

#include <stddef.h>

struct AHeapInfo {
    char unused;
};

static AHeapInfo heap_info;

uint64_t AHeapProfileEnableCallbackInfo_getSamplingInterval(const AHeapProfileEnableCallbackInfo* session_info) {
    (void)session_info;
    return 0;
}

AHeapInfo* AHeapInfo_create(const char* heap_name) {
    return heap_name != NULL ? &heap_info : NULL;
}

AHeapInfo* AHeapInfo_setEnabledCallback(
    AHeapInfo* info, void (*callback)(void* data, const AHeapProfileEnableCallbackInfo* session_info), void* data) {
    (void)callback;
    (void)data;
    return info;
}

AHeapInfo* AHeapInfo_setDisabledCallback(
    AHeapInfo* info, void (*callback)(void* data, const AHeapProfileDisableCallbackInfo* session_info), void* data) {
    (void)callback;
    (void)data;
    return info;
}

// 0 is no heap: heapprofd's IDs start at 1.
uint32_t AHeapProfile_registerHeap(AHeapInfo* info) {
    (void)info;
    return 0;
}

bool AHeapProfile_reportAllocation(uint32_t heap_id, uint64_t alloc_id, uint64_t size) {
    (void)heap_id;
    (void)alloc_id;
    (void)size;
    return false;
}

bool AHeapProfile_reportSample(uint32_t heap_id, uint64_t alloc_id, uint64_t size) {
    (void)heap_id;
    (void)alloc_id;
    (void)size;
    return false;
}

void AHeapProfile_reportFree(uint32_t heap_id, uint64_t alloc_id) {
    (void)heap_id;
    (void)alloc_id;
}
