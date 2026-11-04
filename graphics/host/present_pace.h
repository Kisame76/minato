/* The front libs' presents on MinatoMetalLayer's drawables (ADR-0041 decision 11 c), for angle.m, vulkan.m and
   window.m: each present leaves a record on its window's layer before ANGLE or MoltenVK gets it, in the order of the
   API's calls, and each drawable of the layer takes the oldest record when Metal presents it (layer.m), where
   graphics/host decides whether Core Animation gets it. Plain C. */
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
    /* 1 when decision 11 c)'s rule may withhold the present: EGL's, whose drawables ANGLE lets go after the swap; 0
       for Vulkan's, as MoltenVK keeps a dropped drawable until it acquires that image again (ADR-0041, the sitting
       of 2026-10-04). */
    int ruled;
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

/* Waits until `layer` holds no record of `swapchain`, as when the drawables of its presents reached their present or
   their presents were taken back, or until `deadline` in nanoseconds of the uptime clock: 1 once none is left, 0 at
   the deadline. MoltenVK presents from its present's scheduled handler, which first counts the present on the
   swapchain without checking that it is still there [verified: MVKImage.mm at v1.4.2], and Metal may run that handler
   after the app destroyed it: vulkan.m's destroy waits here before MoltenVK's. */
int minato_present_wait(void *layer, uint64_t swapchain, int64_t deadline);

/* For a drawable of graphics/host's own (window.m): it goes to Core Animation as it comes, without a record. */
void minato_present_unpaced(void *drawable);

/* The window's buffers by contents (window.m, ADR-0043 b5 detail 4): `layer`'s sublayer "minato.window.buffers"
   shows a buffer now, and the next drawable of the layer that goes to Core Animation hides it, as another producer
   took the window. Returns 1 when a drawable of the layer went to Core Animation since the last call, whose frame a
   CPU post covers with a transparent drawable of its own; 0 then, and for a layer that is no MinatoMetalLayer. */
int minato_present_buffers_shown(void *layer);

/* A post of the window's buffers by contents committed at `committed_ns` (CLOCK_UPTIME_RAW): the next place of
   `layer`'s running recording, tick-timed (minato_layer.h's struct minato_layer_drawable), as no drawable gives it a
   presented time (ADR-0043 b5, detail 5). Nothing outside a recording, for a layer that is no MinatoMetalLayer, or
   in the Simulator. */
void minato_present_record_post(void *layer, int64_t committed_ns);

#endif
