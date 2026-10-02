#include "gamecube/card_operation.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static GcCardOperationInput input(int32_t result, bool busy) {
    GcCardOperationInput value = {0};
    value.geometry_active = value.geometry_ready = true;
    for (unsigned slot = 0; slot < 2; ++slot) {
        value.slots[slot].result = result;
        value.slots[slot].worker_busy = busy;
        value.slots[slot].card_ready = true;
    }
    return value;
}

static void sounds(const GcCardOperationEvents *events, const unsigned *expected,
                   size_t count) {
    assert(events->sound_event_count == count);
    for (size_t index = 0; index < count; ++index)
        assert(events->sound_events[index] == expected[index]);
}

static void sound(const GcCardOperationEvents *events, unsigned expected) {
    sounds(events, &expected, 1);
}

static void test_busy_poll_cue(void) {
    for (unsigned slot = 0; slot < 2; ++slot) {
        GcCardOperation operation = {0};
        GcCardOperationEvents events;
        GcCardOperationInput frame = input(GC_CARD_OPERATION_BUSY, true);
        assert(gc_card_operation_begin(&operation, GC_CARD_ACTION_COPY, slot, 9, true,
                                       NULL, &events));
        assert(events.io_requested && events.geometry_begin);
        sound(&events, 8);
        for (unsigned tick = 0; tick < 5; ++tick) {
            assert(gc_card_operation_step(&operation, &frame, &events));
            assert(operation.progress_poll_count == 2 * (tick + 1));
            sounds(&events, NULL, 0);
        }
        assert(gc_card_operation_step(&operation, &frame, &events));
        assert(operation.progress_poll_count == 12);
        sound(&events, slot ? 18 : 17);
        for (unsigned tick = 0; tick < 100; ++tick) {
            assert(gc_card_operation_step(&operation, &frame, &events));
            sounds(&events, NULL, 0);
            assert(!events.dismissed && !operation.input_ready);
        }
        assert(operation.progress_poll_count == 20);
        assert(gc_card_operation_phase(&operation) == GC_CARD_OPERATION_IO);
    }
}

static void settle_layout(GcCardOperation *operation, GcCardOperationInput *frame) {
    GcCardOperationEvents events;
    frame->pressed_buttons = GC_CARD_OPERATION_PAD_A;
    for (unsigned tick = operation->layout_tick; tick < 39; ++tick) {
        assert(gc_card_operation_step(operation, frame, &events));
        assert(!events.dismissed && !operation->input_ready);
    }
    frame->pressed_buttons = 0;
    assert(gc_card_operation_step(operation, frame, &events));
    assert(operation->layout_tick == 40 && operation->input_ready);
    assert(events.presentation_ready);
}

static void test_copy_publication_and_acknowledgment(void) {
    GcCardOperation operation = {0};
    GcCardOperationEvents events;
    GcCardOperationInput frame = input(GC_CARD_OPERATION_BUSY, true);
    assert(gc_card_operation_begin(&operation, GC_CARD_ACTION_COPY, 0, 12, true, NULL,
                                   &events));
    assert(gc_card_operation_step(&operation, &frame, &events));
    frame = input(0, true);
    assert(gc_card_operation_step(&operation, &frame, &events));
    assert(operation.result == 0 && operation.progress_poll_count == 3);
    assert(operation.native_state == GC_CARD_OPERATION_NATIVE_COPY);
    assert(gc_card_operation_phase(&operation) == GC_CARD_OPERATION_METADATA);
    sounds(&events, NULL, 0);
    for (unsigned tick = 0; tick < 25; ++tick) {
        assert(gc_card_operation_step(&operation, &frame, &events));
        assert(!operation.input_ready && operation.selected_special);
        sounds(&events, NULL, 0);
    }
    frame.slots[1].metadata_changed = true;
    frame.slots[1].destination_file_index = 42;
    assert(gc_card_operation_step(&operation, &frame, &events));
    assert(operation.native_state == GC_CARD_OPERATION_NATIVE_COPIED);
    assert(operation.destination_file_index == 42 && !operation.selected_special);
    assert(events.metadata_completed && events.layout_begin &&
           events.metadata_slots == 2);
    assert(operation.layout_tick == 1);
    sound(&events, 19);
    frame.slots[1].metadata_changed = false;
    frame.slots[0].worker_busy = frame.slots[1].worker_busy = false;
    settle_layout(&operation, &frame);
    assert(operation.progress_poll_count == 0);
    assert(gc_card_operation_phase(&operation) == GC_CARD_OPERATION_ACKNOWLEDGE);
    assert(gc_card_operation_step(&operation, &frame, &events));
    assert(!events.presentation_ready);
    sounds(&events, NULL, 0);
    frame.pressed_buttons = GC_CARD_OPERATION_PAD_A;
    assert(gc_card_operation_step(&operation, &frame, &events));
    assert(events.dismissed);
    sound(&events, 7);
    assert(gc_card_operation_phase(&operation) == GC_CARD_OPERATION_IDLE);
}

static void test_move_cues(void) {
    for (unsigned slot = 0; slot < 2; ++slot) {
        GcCardOperation operation = {0};
        GcCardOperationEvents events;
        GcCardOperationInput frame = input(GC_CARD_OPERATION_BUSY, true);
        assert(gc_card_operation_begin(&operation, GC_CARD_ACTION_MOVE, slot, 0, true,
                                       NULL, &events));
        assert(gc_card_operation_step(&operation, &frame, &events));
        frame = input(0, false);
        frame.slots[slot ^ 1].metadata_changed = true;
        assert(gc_card_operation_step(&operation, &frame, &events));
        assert(operation.native_state == GC_CARD_OPERATION_NATIVE_MOVED);
        sound(&events, slot ? 21 : 20);
        frame.slots[slot ^ 1].metadata_changed = false;
        for (unsigned tick = 1; tick < 40; ++tick) {
            assert(gc_card_operation_step(&operation, &frame, &events));
            sounds(&events, NULL, 0);
        }
        assert(operation.input_ready && operation.progress_poll_count == 0);
        frame.pressed_buttons = GC_CARD_OPERATION_PAD_B;
        assert(gc_card_operation_step(&operation, &frame, &events));
        sound(&events, 7);
        assert(events.dismissed);
    }
}

static void test_fast_result_has_no_synthetic_latency(void) {
    GcCardOperation operation = {0};
    GcCardOperationEvents events;
    GcCardOperationInput frame = input(0, false);
    frame.slots[0].metadata_changed = true;
    assert(gc_card_operation_begin(&operation, GC_CARD_ACTION_COPY, 0, 5, true, NULL,
                                   &events));
    assert(gc_card_operation_step(&operation, &frame, &events));
    assert(operation.native_state == GC_CARD_OPERATION_NATIVE_COPIED);
    assert(operation.progress_poll_count == 0);
    sound(&events, 19);
    frame.slots[0].metadata_changed = false;
    for (unsigned tick = 1; tick < 40; ++tick) {
        assert(gc_card_operation_step(&operation, &frame, &events));
        sounds(&events, NULL, 0);
    }
    assert(operation.input_ready);
}

static void test_erase_geometry_and_freeze(void) {
    GcCardOperation operation = {0};
    GcCardOperationEvents events;
    GcCardOperationInput frame = input(GC_CARD_OPERATION_BUSY, true);
    uint8_t delays[GC_CARD_OPERATION_PIECES];
    for (unsigned piece = 0; piece < GC_CARD_OPERATION_PIECES; ++piece)
        delays[piece] = (uint8_t)(piece % 4);
    assert(gc_card_operation_begin(&operation, GC_CARD_ACTION_ERASE, 1, 3, true, delays,
                                   &events));
    assert(operation.erase_active && operation.shrink_remaining == 20);
    float last_scale = 1;
    for (unsigned tick = 0; tick < 10; ++tick) {
        assert(gc_card_operation_step(&operation, &frame, &events));
        assert(operation.shrink_fraction <= last_scale);
        assert(!events.pieces_begin);
        last_scale = operation.shrink_fraction;
    }
    frame.geometry_active = false;
    GcCardOperation frozen = operation;
    for (unsigned tick = 0; tick < 5; ++tick) {
        assert(gc_card_operation_step(&operation, &frame, &events));
        assert(operation.shrink_remaining == frozen.shrink_remaining);
        assert(operation.shrink_fraction == frozen.shrink_fraction);
    }
    frame.geometry_active = true;
    for (unsigned tick = 10; tick < 20; ++tick)
        assert(gc_card_operation_step(&operation, &frame, &events));
    assert(operation.shrink_remaining == 0 && operation.selected_special);
    for (unsigned tick = 0; tick < 10; ++tick) {
        assert(gc_card_operation_step(&operation, &frame, &events));
        assert(operation.shrink_fraction == 0 && !operation.pieces_active);
    }
    frame = input(0, false);
    frame.slots[1].metadata_changed = true;
    assert(gc_card_operation_step(&operation, &frame, &events));
    assert(events.metadata_completed && events.pieces_begin && events.layout_begin);
    assert(operation.native_state == GC_CARD_OPERATION_NATIVE_ERASED);
    assert(operation.pieces_active && operation.layout_tick == 0);
    for (unsigned piece = 0; piece < GC_CARD_OPERATION_PIECES; ++piece)
        assert(operation.piece_ticks[piece] == 1);
    frame.slots[1].metadata_changed = false;
    for (unsigned tick = 2; tick <= 13; ++tick) {
        frame.pressed_buttons = GC_CARD_OPERATION_PAD_A;
        assert(gc_card_operation_step(&operation, &frame, &events));
        assert(operation.pieces_active && operation.layout_tick == 0);
        assert(!events.dismissed && !events.pieces_finished);
    }
    assert(gc_card_operation_step(&operation, &frame, &events));
    assert(events.pieces_finished && !operation.erase_active &&
           !operation.pieces_active);
    assert(operation.layout_tick == 0);
    for (unsigned tick = 1; tick < 40; ++tick) {
        assert(gc_card_operation_step(&operation, &frame, &events));
        assert(!events.dismissed);
        assert(operation.layout_tick == tick);
    }
    frame.pressed_buttons = 0;
    assert(gc_card_operation_step(&operation, &frame, &events));
    assert(events.presentation_ready && operation.input_ready);
    sound(&events, 15);
    assert(gc_card_operation_step(&operation, &frame, &events));
    sounds(&events, NULL, 0);
    frame.pressed_buttons = GC_CARD_OPERATION_PAD_B;
    assert(gc_card_operation_step(&operation, &frame, &events));
    assert(events.dismissed);
    sound(&events, 7);
}

static void test_early_erase_success(void) {
    GcCardOperation operation = {0};
    GcCardOperationEvents events;
    GcCardOperationInput frame = input(0, false);
    frame.slots[0].metadata_changed = true;
    assert(gc_card_operation_begin(&operation, GC_CARD_ACTION_ERASE, 0, 2, true, NULL,
                                   &events));
    assert(gc_card_operation_step(&operation, &frame, &events));
    frame.slots[0].metadata_changed = false;
    for (unsigned tick = 2; tick <= 20; ++tick) {
        assert(gc_card_operation_step(&operation, &frame, &events));
        assert(!events.pieces_begin && !operation.pieces_active);
    }
    assert(operation.shrink_remaining == 0 && operation.erase_active);
    assert(gc_card_operation_step(&operation, &frame, &events));
    assert(events.pieces_begin && operation.piece_ticks[0] == 1);
    for (unsigned tick = 2; tick <= 10; ++tick) {
        assert(gc_card_operation_step(&operation, &frame, &events));
        assert(!events.pieces_finished);
    }
    assert(gc_card_operation_step(&operation, &frame, &events));
    assert(events.pieces_finished);
    assert(operation.piece_ticks[0] == 10 && operation.layout_tick == 0);
}

static void test_errors_and_format(void) {
    GcCardOperation operation = {0};
    GcCardOperationEvents events;
    GcCardOperationInput frame = input(-9, false);
    assert(gc_card_operation_begin(&operation, GC_CARD_ACTION_ERASE, 0, 1, true, NULL,
                                   &events));
    assert(gc_card_operation_step(&operation, &frame, &events));
    assert(events.geometry_aborted && events.layout_begin);
    assert(!operation.erase_active && !operation.selected_special);
    assert(operation.native_state == GC_CARD_OPERATION_NATIVE_ERASED &&
           operation.result == -9);
    sound(&events, 13);
    for (unsigned tick = 1; tick < 40; ++tick) {
        assert(gc_card_operation_step(&operation, &frame, &events));
        sounds(&events, NULL, 0);
    }
    assert(operation.input_ready);
    frame.pressed_buttons = GC_CARD_OPERATION_PAD_A | GC_CARD_OPERATION_PAD_B;
    assert(gc_card_operation_step(&operation, &frame, &events));
    const unsigned both[2] = {7, 7};
    sounds(&events, both, 2);
    assert(events.dismissed);
    assert(gc_card_operation_begin(&operation, GC_CARD_ACTION_MOVE, 1, 1, false, NULL,
                                   &events));
    assert(!events.io_requested && !events.geometry_begin);
    sounds(&events, NULL, 0);
    frame.pressed_buttons = 0;
    assert(gc_card_operation_step(&operation, &frame, &events));
    sound(&events, 13);
    assert(operation.result == GC_CARD_OPERATION_NO_DESTINATION);
    frame.pressed_buttons = GC_CARD_OPERATION_PAD_A;
    assert(gc_card_operation_step(&operation, &frame, &events));
    assert(events.dismissed);
    assert(gc_card_operation_begin(&operation, GC_CARD_ACTION_FORMAT, 1, 0, false, NULL,
                                   &events));
    assert(events.io_requested && !events.geometry_begin);
    frame = input(GC_CARD_OPERATION_BUSY, true);
    for (unsigned tick = 0; tick < 25; ++tick) {
        assert(gc_card_operation_step(&operation, &frame, &events));
        assert(operation.progress_poll_count == 0 && !operation.input_ready);
        sounds(&events, NULL, 0);
    }
    frame = input(0, false);
    frame.geometry_ready = false;
    assert(gc_card_operation_step(&operation, &frame, &events));
    assert(operation.native_state == GC_CARD_OPERATION_NATIVE_FORMATTED);
    assert(!operation.input_ready);
    sound(&events, 16);
    for (unsigned tick = 0; tick < 10; ++tick) {
        assert(gc_card_operation_step(&operation, &frame, &events));
        sounds(&events, NULL, 0);
    }
    frame.geometry_ready = true;
    assert(gc_card_operation_step(&operation, &frame, &events));
    assert(operation.input_ready && events.presentation_ready);
    sounds(&events, NULL, 0);
}

typedef struct {
    GcCardOperationInput input;
    unsigned calls;
    bool fail_second;
} Adapter;

static bool poll_adapter(void *context, unsigned slot, GcCardOperationPoll *poll) {
    Adapter *adapter = context;
    assert(slot == adapter->calls % 2);
    ++adapter->calls;
    if (slot && adapter->fail_second)
        return false;
    *poll = adapter->input.slots[slot];
    return true;
}

static void test_adapter_and_validation(void) {
    GcCardOperation operation = {0};
    GcCardOperationEvents events;
    assert(
        !gc_card_operation_begin(NULL, GC_CARD_ACTION_COPY, 0, 0, true, NULL, &events));
    assert(!gc_card_operation_begin(&operation, GC_CARD_ACTION_COPY, 2, 0, true, NULL,
                                    &events));
    assert(!gc_card_operation_begin(&operation, GC_CARD_ACTION_ERASE, 0, 127, true,
                                    NULL, &events));
    uint8_t delays[12] = {4};
    assert(!gc_card_operation_begin(&operation, GC_CARD_ACTION_ERASE, 0, 1, true,
                                    delays, &events));
    assert(gc_card_operation_begin(&operation, GC_CARD_ACTION_COPY, 0, 1, true, NULL,
                                   &events));
    assert(!gc_card_operation_begin(&operation, GC_CARD_ACTION_COPY, 0, 1, true, NULL,
                                    &events));
    Adapter adapter = {0};
    adapter.input = input(GC_CARD_OPERATION_BUSY, true);
    assert(gc_card_operation_step_adapter(&operation, poll_adapter, &adapter, true,
                                          true, GC_CARD_OPERATION_PAD_A, &events));
    assert(adapter.calls == 2 && operation.progress_poll_count == 2);
    assert(!events.dismissed);
    adapter.fail_second = true;
    GcCardOperation saved = operation;
    assert(!gc_card_operation_step_adapter(&operation, poll_adapter, &adapter, true,
                                           true, 0, &events));
    assert(!memcmp(&operation, &saved, sizeof(operation)));
    adapter.input.slots[0].metadata_changed = true;
    adapter.input.slots[0].destination_file_index = 127;
    assert(!gc_card_operation_step(&operation, &adapter.input, &events));
    assert(!memcmp(&operation, &saved, sizeof(operation)));
}

int main(void) {
    test_busy_poll_cue();
    test_copy_publication_and_acknowledgment();
    test_move_cues();
    test_fast_result_has_no_synthetic_latency();
    test_erase_geometry_and_freeze();
    test_early_erase_success();
    test_errors_and_format();
    test_adapter_and_validation();
    puts("card operation controller tests passed");
    return 0;
}
