#include "gamecube/card_runtime.h"

#include <limits.h>
#include <math.h>
#include <string.h>

static bool final_confirmation(const gc_menu *menu) {
    return menu->page == GC_PAGE_CARD_CONFIRM && menu->confirm_yes &&
           (menu->card_action != GC_CARD_ACTION_FORMAT ||
            menu->format_second_confirmation);
}

bool gc_card_runtime_init(GcCardRuntime *runtime, GcServices *services, gc_menu *menu,
                          GcScene *scene) {
    if (!runtime || !services || !menu)
        return false;
    *runtime = (GcCardRuntime){0};
    runtime->services = services;
    runtime->menu = menu;
    runtime->scene = scene;
    runtime->frame_rate = menu->region == GC_REGION_EUROPE ? 50 : 60;
    return true;
}

void gc_card_runtime_destroy(GcCardRuntime *runtime) {
    if (!runtime)
        return;
    if (runtime->scene)
        gc_scene_card_operation_end(runtime->scene);
    *runtime = (GcCardRuntime){0};
}

void gc_card_runtime_set_random(GcCardRuntime *runtime,
                                GcCardRuntimeRandomAdapter adapter, void *context) {
    if (runtime) {
        runtime->random = adapter;
        runtime->random_context = context;
    }
}

static bool same_file(const gc_card_file *first, const gc_card_file *second) {
    return !memcmp(first->game_code, second->game_code, sizeof(first->game_code)) &&
           !memcmp(first->maker_code, second->maker_code, sizeof(first->maker_code)) &&
           !strcmp(first->filename, second->filename);
}

bool gc_card_runtime_begin(GcCardRuntime *runtime, const uint8_t piece_delays[12],
                           const int16_t piece_angles[12],
                           GcCardRuntimeEvents *events) {
    if (!runtime || !runtime->menu || !runtime->services || !events ||
        runtime->active || !final_confirmation(runtime->menu))
        return false;
    gc_menu *menu = runtime->menu;
    *events = (GcCardRuntimeEvents){0};
    unsigned slot = menu->card_slot;
    if (slot > 1 || menu->card_index > UINT_MAX)
        return false;
    gc_card_file selected = {0};
    const gc_card_file *file = gc_menu_card_selected(menu);
    if (file)
        selected = *file;
    if (!gc_card_operation_begin(&runtime->operation, menu->card_action, slot,
                                 (unsigned)menu->card_index,
                                 menu->cards[slot ^ 1].status == GC_CARD_READY,
                                 piece_delays, &events->operation))
        return false;
    if (runtime->random && runtime->operation.selected_special &&
        (menu->card_action == GC_CARD_ACTION_COPY ||
         menu->card_action == GC_CARD_ACTION_MOVE)) {
        /* Native 1efbc samples the first special-path angle at capture,
         * even when the local IO result arrives on the next video update. */
        uint32_t angle;
        if (!runtime->random(runtime->random_context, &angle)) {
            runtime->operation = (GcCardOperation){0};
            *events = (GcCardRuntimeEvents){0};
            return false;
        }
    }
    if (runtime->scene &&
        !gc_scene_card_operation_begin(runtime->scene, menu, &runtime->operation,
                                       piece_angles)) {
        runtime->operation = (GcCardOperation){0};
        *events = (GcCardRuntimeEvents){0};
        return false;
    }
    uint64_t revision = runtime->services->revision;
    gc_services_press(runtime->services, menu, GC_BUTTON_CONFIRM);
    if (menu->page != GC_PAGE_MESSAGE) {
        if (runtime->scene)
            gc_scene_card_operation_end(runtime->scene);
        runtime->operation = (GcCardOperation){0};
        return false;
    }
    bool success = menu->message == GC_MESSAGE_CARD_ERASED ||
                   menu->message == GC_MESSAGE_CARD_COPIED ||
                   menu->message == GC_MESSAGE_CARD_MOVED ||
                   menu->message == GC_MESSAGE_CARD_FORMATTED;
    /* The portable service retains its own precise gc_message. Its failure
     * sentinel need only distinguish a local error from original BUSY/OK.
     */
    runtime->adapter_result = success ? 0 : -100;
    runtime->metadata_pending = 0;
    if (runtime->services->revision != revision) {
        gc_card_action action = runtime->operation.action;
        runtime->metadata_pending =
            action == GC_CARD_ACTION_MOVE
                ? 3
                : (uint8_t)(1u << (action == GC_CARD_ACTION_COPY ? slot ^ 1 : slot));
    }
    runtime->destination_file_index = 0;
    if (success && file &&
        (runtime->operation.action == GC_CARD_ACTION_COPY ||
         runtime->operation.action == GC_CARD_ACTION_MOVE)) {
        const gc_card *destination = &menu->cards[slot ^ 1];
        for (size_t index = 0; index < destination->file_count; ++index) {
            if (same_file(&selected, &destination->files[index])) {
                runtime->destination_file_index = (unsigned)index;
                break;
            }
        }
    }
    runtime->active = true;
    runtime->random_pending = runtime->operation.action == GC_CARD_ACTION_ERASE &&
                              runtime->random && !piece_delays && !piece_angles;
    runtime->fraction = 0;
    events->handled = true;
    return true;
}

bool gc_card_runtime_press(GcCardRuntime *runtime, gc_button button,
                           const uint8_t piece_delays[12],
                           const int16_t piece_angles[12],
                           GcCardRuntimeEvents *events) {
    if (!runtime || !runtime->menu || !runtime->services || !events)
        return false;
    *events = (GcCardRuntimeEvents){0};
    if (runtime->active) {
        events->handled = true;
        uint16_t pressed = button == GC_BUTTON_CONFIRM  ? GC_CARD_OPERATION_PAD_A
                           : button == GC_BUTTON_CANCEL ? GC_CARD_OPERATION_PAD_B
                                                        : 0;
        if (!gc_card_operation_press(&runtime->operation, pressed, &events->operation))
            return false;
        if (events->operation.dismissed) {
            gc_services_press(runtime->services, runtime->menu, button);
            runtime->active = false;
            if (runtime->scene)
                gc_scene_card_operation_end(runtime->scene);
        }
        return true;
    }
    if (button == GC_BUTTON_CONFIRM && final_confirmation(runtime->menu))
        return gc_card_runtime_begin(runtime, piece_delays, piece_angles, events);
    return true;
}

static bool sample_pieces(GcCardRuntime *runtime) {
    uint8_t delays[12];
    int16_t angles[12];
    for (unsigned piece = 0; piece < 12; ++piece) {
        uint32_t value;
        if (!runtime->random(runtime->random_context, &value))
            return false;
        delays[piece] = (uint8_t)(value & 3);
        if (!runtime->random(runtime->random_context, &value))
            return false;
        uint16_t bits = (uint16_t)value;
        angles[piece] = (int16_t)(bits <= INT16_MAX ? (int)bits : (int)bits - 65536);
    }
    memcpy(runtime->operation.piece_delays, delays, sizeof(delays));
    if (runtime->scene)
        memcpy(runtime->scene->card_erase_angles, angles, sizeof(angles));
    runtime->random_pending = false;
    return true;
}

static void append_events(GcCardOperationEvents *combined,
                          const GcCardOperationEvents *tick) {
    for (unsigned index = 0; index < tick->sound_event_count; ++index) {
        if (combined->sound_event_count < 8)
            combined->sound_events[combined->sound_event_count++] =
                tick->sound_events[index];
    }
    combined->geometry_aborted |= tick->geometry_aborted;
    combined->metadata_completed |= tick->metadata_completed;
    combined->layout_begin |= tick->layout_begin;
    combined->pieces_begin |= tick->pieces_begin;
    combined->pieces_finished |= tick->pieces_finished;
    combined->presentation_ready |= tick->presentation_ready;
    combined->dismissed |= tick->dismissed;
    combined->metadata_slots |= tick->metadata_slots;
}

bool gc_card_runtime_tick(GcCardRuntime *runtime, unsigned video_ticks,
                          GcCardRuntimeEvents *events) {
    if (!runtime || !runtime->menu || !runtime->services || !events)
        return false;
    *events = (GcCardRuntimeEvents){0};
    if (!runtime->active)
        return true;
    events->handled = true;
    for (unsigned tick = 0; tick < video_ticks; ++tick) {
        if (runtime->random_pending && runtime->operation.erase_active &&
            !runtime->operation.shrink_remaining &&
            (!runtime->operation.selected_special ||
             (runtime->metadata_pending && !runtime->adapter_result)) &&
            !sample_pieces(runtime))
            return false;
        GcCardOperationInput input = {0};
        input.geometry_active = true;
        input.geometry_ready =
            !runtime->scene || gc_scene_card_operation_ready(runtime->scene);
        for (unsigned slot = 0; slot < 2; ++slot) {
            input.slots[slot].result = runtime->adapter_result;
            input.slots[slot].metadata_changed =
                (runtime->metadata_pending & (1u << slot)) != 0;
            input.slots[slot].card_ready =
                runtime->menu->cards[slot].status == GC_CARD_READY;
            input.slots[slot].destination_file_index =
                (uint8_t)runtime->destination_file_index;
        }
        GcCardOperationEvents result;
        if (!gc_card_operation_step(&runtime->operation, &input, &result))
            return false;
        runtime->metadata_pending = 0;
        append_events(&events->operation, &result);
        if (runtime->scene && result.layout_begin &&
            !gc_scene_card_relayout(runtime->scene, runtime->menu, &runtime->operation))
            return false;
        if (runtime->scene)
            gc_scene_card_operation_update(runtime->scene, &runtime->operation);
        /* The synchronous local adapter has no further IO or geometry work
         * once presentation is ready. Retain readiness for the next key edge.
         */
        if (runtime->operation.input_ready)
            break;
    }
    return true;
}

bool gc_card_runtime_advance(GcCardRuntime *runtime, double elapsed_seconds,
                             GcCardRuntimeEvents *events) {
    if (!runtime || !events || !isfinite(elapsed_seconds) || elapsed_seconds < 0 ||
        !runtime->frame_rate)
        return false;
    double total = runtime->fraction + elapsed_seconds * runtime->frame_rate;
    if (!isfinite(total) || total > UINT_MAX)
        return false;
    unsigned ticks = (unsigned)floor(total);
    runtime->fraction = total - ticks;
    return gc_card_runtime_tick(runtime, ticks, events);
}
