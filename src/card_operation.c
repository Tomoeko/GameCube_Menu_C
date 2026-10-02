#include "gamecube/card_operation.h"

#include <string.h>

/* USA/JAP16b0c/16950/16690 and EUR17868/176ac/173ec use the same counters,
 * status codes and cues. USA16e24 / EUR17b80 call the poll once per slot.
 * Erase20-tick shrink and strict particle expiry are USA1efbc/1e820;
 * relayout40 and the ready gate are USA1f494/1f9e4, EUR2056c/20abc.
 */
static bool pending_file_operation(GcCardOperationNativeState state) {
    return state >= GC_CARD_OPERATION_NATIVE_COPY &&
           state <= GC_CARD_OPERATION_NATIVE_ERASE;
}

static bool transfer(gc_card_action action) {
    return action == GC_CARD_ACTION_COPY || action == GC_CARD_ACTION_MOVE;
}

static void cue(GcCardOperationEvents *events, unsigned sound) {
    if (events->sound_event_count < 8)
        events->sound_events[events->sound_event_count++] = sound;
}

static GcCardOperationNativeState finished(GcCardOperationNativeState state) {
    switch (state) {
        case GC_CARD_OPERATION_NATIVE_COPY:
            return GC_CARD_OPERATION_NATIVE_COPIED;
        case GC_CARD_OPERATION_NATIVE_MOVE:
            return GC_CARD_OPERATION_NATIVE_MOVED;
        case GC_CARD_OPERATION_NATIVE_ERASE:
            return GC_CARD_OPERATION_NATIVE_ERASED;
        default:
            return GC_CARD_OPERATION_NATIVE_FORMATTED;
    }
}

static void relayout(GcCardOperation *operation, GcCardOperationEvents *events) {
    operation->layout_tick = 0;
    operation->layout_active = true;
    operation->input_ready = false;
    events->layout_begin = true;
}

static void abort_geometry(GcCardOperation *operation, GcCardOperationEvents *events) {
    /* Native1f3d8 restores the selected record only while its special flag
     * remains set. Repeated notifications cannot restart that restoration.
     */
    if (!operation->selected_special)
        return;
    operation->selected_special = false;
    operation->erase_active = false;
    operation->pieces_active = false;
    events->geometry_aborted = true;
    relayout(operation, events);
}

bool gc_card_operation_begin(GcCardOperation *operation, gc_card_action action,
                             unsigned source_slot, unsigned source_file_index,
                             bool destination_ready,
                             const uint8_t piece_delays[GC_CARD_OPERATION_PIECES],
                             GcCardOperationEvents *events) {
    if (!operation || !events ||
        operation->native_state != GC_CARD_OPERATION_NATIVE_IDLE ||
        action < GC_CARD_ACTION_MOVE || action > GC_CARD_ACTION_FORMAT ||
        source_slot > 1 ||
        (action != GC_CARD_ACTION_FORMAT && source_file_index >= GC_CARD_FILE_LIMIT))
        return false;
    if (piece_delays) {
        for (unsigned piece = 0; piece < GC_CARD_OPERATION_PIECES; ++piece) {
            if (piece_delays[piece] > 3)
                return false;
        }
    }
    *operation = (GcCardOperation){0};
    *events = (GcCardOperationEvents){0};
    operation->action = action;
    operation->source_slot = source_slot;
    operation->source_file_index = source_file_index;
    operation->result = GC_CARD_OPERATION_BUSY;
    operation->layout_tick = GC_CARD_OPERATION_LAYOUT_TICKS;
    if (piece_delays)
        memcpy(operation->piece_delays, piece_delays, sizeof(operation->piece_delays));
    switch (action) {
        case GC_CARD_ACTION_FORMAT:
            operation->native_state = GC_CARD_OPERATION_NATIVE_FORMAT;
            break;
        case GC_CARD_ACTION_COPY:
            operation->native_state = GC_CARD_OPERATION_NATIVE_COPY;
            break;
        case GC_CARD_ACTION_MOVE:
            operation->native_state = GC_CARD_OPERATION_NATIVE_MOVE;
            break;
        case GC_CARD_ACTION_ERASE:
            operation->native_state = GC_CARD_OPERATION_NATIVE_ERASE;
            break;
    }
    if (transfer(action) && !destination_ready) {
        operation->result = GC_CARD_OPERATION_NO_DESTINATION;
        return true;
    }
    events->io_requested = true;
    cue(events, 8);
    if (action != GC_CARD_ACTION_FORMAT) {
        operation->selected_special = true;
        events->geometry_begin = true;
    }
    if (action == GC_CARD_ACTION_ERASE) {
        operation->erase_active = true;
        operation->shrink_remaining = GC_CARD_OPERATION_SHRINK_TICKS;
        operation->shrink_fraction = 1;
    }
    return true;
}

static void metadata(GcCardOperation *operation, const GcCardOperationPoll *poll,
                     unsigned slot, GcCardOperationEvents *events) {
    if (!poll->metadata_changed || !poll->card_ready)
        return;
    events->metadata_slots |= (uint8_t)(1u << slot);
    if (pending_file_operation(operation->native_state)) {
        if (operation->result == GC_CARD_OPERATION_BUSY)
            operation->result = poll->result;
        if (operation->result) {
            abort_geometry(operation, events);
        } else {
            operation->selected_special = false;
            operation->destination_file_index = poll->destination_file_index;
            events->metadata_completed = true;
            if (operation->native_state == GC_CARD_OPERATION_NATIVE_COPY)
                cue(events, 19);
            else if (operation->native_state == GC_CARD_OPERATION_NATIVE_MOVE)
                cue(events, operation->source_slot ? 21 : 20);
            operation->native_state = finished(operation->native_state);
        }
    }
    relayout(operation, events);
}

static void poll_operation(GcCardOperation *operation, const GcCardOperationPoll *poll,
                           GcCardOperationEvents *events) {
    if (pending_file_operation(operation->native_state)) {
        if (operation->result == GC_CARD_OPERATION_BUSY) {
            if (transfer(operation->action) && operation->progress_poll_count == 10)
                cue(events, operation->source_slot ? 18 : 17);
            if (operation->progress_poll_count < 20)
                ++operation->progress_poll_count;
            operation->result = poll->result;
        }
        if (operation->result != GC_CARD_OPERATION_BUSY && operation->result) {
            operation->native_state = finished(operation->native_state);
            abort_geometry(operation, events);
            cue(events, 13);
        }
    } else if (operation->native_state == GC_CARD_OPERATION_NATIVE_FORMAT) {
        if (operation->result == GC_CARD_OPERATION_BUSY)
            operation->result = poll->result;
        if (operation->result != GC_CARD_OPERATION_BUSY) {
            cue(events, operation->result ? 13 : 16);
            operation->native_state = GC_CARD_OPERATION_NATIVE_FORMATTED;
        }
    }
}

static void erase_geometry(GcCardOperation *operation, GcCardOperationEvents *events) {
    float fraction =
        (float)operation->shrink_remaining / GC_CARD_OPERATION_SHRINK_TICKS;
    operation->shrink_fraction = fraction * fraction * (3 - 2 * fraction);
    if (operation->shrink_remaining) {
        --operation->shrink_remaining;
        return;
    }
    /* A busy erase keeps its special card at scale0. Native particles begin
     * only after successful metadata publication clears that special flag.
     */
    if (operation->selected_special)
        return;
    if (!operation->pieces_active) {
        operation->pieces_active = true;
        events->pieces_begin = true;
    }
    unsigned complete = 0;
    for (unsigned piece = 0; piece < GC_CARD_OPERATION_PIECES; ++piece) {
        unsigned tick = (unsigned)operation->piece_ticks[piece] + 1;
        unsigned limit = GC_CARD_OPERATION_PIECE_TICKS + operation->piece_delays[piece];
        if (tick > limit) {
            tick = limit;
            ++complete;
        }
        operation->piece_ticks[piece] = (uint8_t)tick;
    }
    if (complete == GC_CARD_OPERATION_PIECES) {
        operation->pieces_active = false;
        operation->erase_active = false;
        events->pieces_finished = true;
    }
}

static void acknowledge(GcCardOperation *operation, uint16_t pressed_buttons,
                        GcCardOperationEvents *events) {
    if (operation->progress_poll_count) {
        if (!operation->result && operation->action == GC_CARD_ACTION_COPY)
            cue(events, 16);
        else if (!operation->result && operation->action == GC_CARD_ACTION_ERASE)
            cue(events, 15);
    }
    operation->progress_poll_count = 0;
    /* The native dispatcher tests A and B independently. Its input sampler
     * suppresses A when B was pressed, but raw inspection can retain both.
     */
    const uint16_t buttons[2] = {GC_CARD_OPERATION_PAD_A, GC_CARD_OPERATION_PAD_B};
    for (unsigned index = 0; index < 2; ++index) {
        if (!(pressed_buttons & buttons[index]))
            continue;
        operation->native_state = GC_CARD_OPERATION_NATIVE_IDLE;
        operation->result = 0;
        operation->input_ready = false;
        events->dismissed = true;
        cue(events, 7);
    }
}

bool gc_card_operation_step(GcCardOperation *operation,
                            const GcCardOperationInput *input,
                            GcCardOperationEvents *events) {
    if (!operation || !input || !events ||
        operation->native_state < GC_CARD_OPERATION_NATIVE_IDLE ||
        operation->native_state > GC_CARD_OPERATION_NATIVE_FORMATTED)
        return false;
    for (unsigned slot = 0; slot < 2; ++slot) {
        if (input->slots[slot].metadata_changed && input->slots[slot].card_ready &&
            input->slots[slot].destination_file_index >= GC_CARD_FILE_LIMIT)
            return false;
    }
    *events = (GcCardOperationEvents){0};
    if (operation->native_state == GC_CARD_OPERATION_NATIVE_IDLE)
        return true;
    for (unsigned slot = 0; slot < 2; ++slot) {
        metadata(operation, &input->slots[slot], slot, events);
        poll_operation(operation, &input->slots[slot], events);
    }
    if (input->geometry_active) {
        if (operation->erase_active) {
            erase_geometry(operation, events);
        } else if (operation->layout_active) {
            if (operation->layout_tick < GC_CARD_OPERATION_LAYOUT_TICKS)
                ++operation->layout_tick;
            if (operation->layout_tick == GC_CARD_OPERATION_LAYOUT_TICKS)
                operation->layout_active = false;
        }
    }
    bool ready = input->geometry_ready && !input->slots[1].worker_busy &&
                 !operation->erase_active && !operation->layout_active;
    events->presentation_ready = ready && !operation->input_ready;
    operation->input_ready = ready;
    if (ready)
        acknowledge(operation, input->pressed_buttons, events);
    return true;
}

GcCardOperationPhase gc_card_operation_phase(const GcCardOperation *operation) {
    if (!operation || operation->native_state == GC_CARD_OPERATION_NATIVE_IDLE)
        return GC_CARD_OPERATION_IDLE;
    if (operation->input_ready)
        return GC_CARD_OPERATION_ACKNOWLEDGE;
    if (operation->result == GC_CARD_OPERATION_BUSY)
        return GC_CARD_OPERATION_IO;
    if (pending_file_operation(operation->native_state) && !operation->result)
        return GC_CARD_OPERATION_METADATA;
    return GC_CARD_OPERATION_GEOMETRY;
}

bool gc_card_operation_press(GcCardOperation *operation, uint16_t pressed_buttons,
                             GcCardOperationEvents *events) {
    if (!operation || !events)
        return false;
    *events = (GcCardOperationEvents){0};
    if (operation->native_state != GC_CARD_OPERATION_NATIVE_IDLE &&
        operation->input_ready)
        acknowledge(operation, pressed_buttons, events);
    return true;
}

bool gc_card_operation_step_adapter(GcCardOperation *operation,
                                    GcCardOperationPollAdapter adapter, void *context,
                                    bool geometry_active, bool geometry_ready,
                                    uint16_t pressed_buttons,
                                    GcCardOperationEvents *events) {
    if (!operation || !adapter || !events)
        return false;
    GcCardOperationInput input = {0};
    input.geometry_active = geometry_active;
    input.geometry_ready = geometry_ready;
    input.pressed_buttons = pressed_buttons;
    for (unsigned slot = 0; slot < 2; ++slot) {
        if (!adapter(context, slot, &input.slots[slot]))
            return false;
    }
    return gc_card_operation_step(operation, &input, events);
}
