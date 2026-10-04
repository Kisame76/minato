/* The front libs' presents on MinatoMetalLayer's drawables (ADR-0041 decision 11 c), plan
   docs/plans/adr-0041-11c-frame-drop.md), for angle.m, vulkan.m and window.m: each present leaves a record on its
   window's layer before ANGLE or MoltenVK gets it, in the order of the API's calls, and each drawable of the layer
   takes the oldest record when Metal presents it (layer.m), where graphics/host decides whether Core Animation gets
   it. Plain C. */
#ifndef MINATO_PRESENT_PACE_H
#define MINATO_PRESENT_PACE_H

#include <stdint.h>

/* A present's record. */
struct minato_present_record {
    /* The time its app asked for, in nanoseconds of the uptime clock; 0 for none. */
    int64_t asked;
    /* Called with the record, on the thread that presents the drawable, when Core Animation does not get it; NULL
       for none. The other fields are its caller's. */
    void (*dropped)(const struct minato_present_record *record);
    uint64_t swapchain;
    uint64_t handed;
    uint32_t id;
};

/* Leaves the record on `layer` (a CAMetalLayer, unretained) for the next of its drawables Metal presents. Returns a
   ticket for minato_present_take_back, 0 when the layer is no MinatoMetalLayer: its drawables then go to Core
   Animation as they come. */
uint64_t minato_present_push(void *layer, const struct minato_present_record *record);

/* Takes the record with `ticket` back, when its present failed before ANGLE or MoltenVK presented a drawable. */
void minato_present_take_back(void *layer, uint64_t ticket);

/* Forgets the layer's records, when the surface or swapchain on it goes: drawables still on their way go to Core
   Animation as they come. */
void minato_present_forget(void *layer);

/* For a drawable of graphics/host's own (window.m): it goes to Core Animation as it comes, without a record. */
void minato_present_unpaced(void *drawable);

#endif
