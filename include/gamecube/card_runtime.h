#ifndef GAMECUBE_CARD_RUNTIME_H
#define GAMECUBE_CARD_RUNTIME_H

#include "gamecube/card_operation.h"
#include "gamecube/services.h"
#include "gamecube/render.h"

typedef bool (*GcCardRuntimeRandomAdapter)(void *context, uint32_t *value);

typedef struct {
    /* Borrowed: the service, menu and optional scene outlive this controller. */
    GcServices *services;
    gc_menu *menu;
    GcScene *scene;
    GcCardOperation operation;
    unsigned frame_rate;
    double fraction;
    int32_t adapter_result;
    uint8_t metadata_pending;
    unsigned destination_file_index;
    bool active;
    GcCardRuntimeRandomAdapter random;
    void *random_context;
    bool random_pending;
} GcCardRuntime;

typedef struct {
    /* True when the operation consumed this input; skip generic menu cues. */
    bool handled;
    GcCardOperationEvents operation;
} GcCardRuntimeEvents;

bool gc_card_runtime_init(GcCardRuntime *runtime, GcServices *services, gc_menu *menu,
                          GcScene *scene);
void gc_card_runtime_destroy(GcCardRuntime *runtime);
void gc_card_runtime_set_random(GcCardRuntime *runtime,
                                GcCardRuntimeRandomAdapter adapter, void *context);

/* Begins only the final confirmed Yes action. The existing local service
 * writes its shadow images synchronously. Its actual result is published to
 * the recovered controller on the next video update, without a fake BUSY
 * interval. The optional scene retains the original selected art/transform.
 */
bool gc_card_runtime_begin(GcCardRuntime *runtime, const uint8_t piece_delays[12],
                           const int16_t piece_angles[12], GcCardRuntimeEvents *events);

/* Ordinary input returns handled=false without changing the menu. While an
 * operation is active, A/B are consumed until its presentation gate is ready.
 */
bool gc_card_runtime_press(GcCardRuntime *runtime, gc_button button,
                           const uint8_t piece_delays[12],
                           const int16_t piece_angles[12], GcCardRuntimeEvents *events);
bool gc_card_runtime_tick(GcCardRuntime *runtime, unsigned video_ticks,
                          GcCardRuntimeEvents *events);
bool gc_card_runtime_advance(GcCardRuntime *runtime, double elapsed_seconds,
                             GcCardRuntimeEvents *events);

#endif
