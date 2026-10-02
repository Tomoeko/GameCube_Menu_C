#include "gamecube/error_control.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>

static void test_fade_boundaries_and_pause(bool startup) {
    GcErrorControl state = {0};
    assert(gc_error_control_alpha(&state) == 0);
    gc_error_control_init(&state, startup);
    unsigned limit = startup ? 30u : 20u;
    unsigned first_alpha = startup ? 8u : 12u;
    unsigned penultimate_alpha = startup ? 246u : 242u;
    assert(!state.requested && state.fade_ticks == limit && !state.ticks);
    assert(gc_error_control_toggle(&state));
    assert(gc_error_control_advance(&state, 0));
    assert(state.requested && state.ticks == 0);
    assert(gc_error_control_advance(&state, 1));
    assert(state.ticks == 1 && gc_error_control_alpha(&state) == first_alpha);
    assert(gc_error_control_advance(&state, limit / 2 - 1));
    assert(state.ticks == limit / 2 && gc_error_control_alpha(&state) == 127);
    assert(gc_error_control_advance(&state, limit / 2 - 1));
    assert(state.ticks == limit - 1 &&
           gc_error_control_alpha(&state) == penultimate_alpha);
    assert(gc_error_control_advance(&state, 1));
    assert(state.ticks == limit && gc_error_control_alpha(&state) == 255);
    assert(gc_error_control_advance(&state, UINT64_MAX));
    assert(state.ticks == limit && gc_error_control_alpha(&state) == 255);
    assert(gc_error_control_toggle(&state));
    assert(gc_error_control_advance(&state, 0));
    assert(!state.requested && state.ticks == limit);
    assert(gc_error_control_advance(&state, limit - 1));
    assert(state.ticks == 1 && gc_error_control_alpha(&state) == first_alpha);
    assert(gc_error_control_advance(&state, UINT64_MAX));
    assert(!state.ticks && !gc_error_control_alpha(&state));
    assert(state.fade_ticks == limit);
}

static void test_reversal_and_replay(bool startup) {
    GcErrorControl state;
    gc_error_control_init(&state, startup);
    unsigned limit = startup ? 30u : 20u;
    assert(gc_error_control_toggle(&state));
    assert(gc_error_control_advance(&state, 11));
    GcErrorControl saved = state;
    assert(gc_error_control_toggle(&state));
    assert(gc_error_control_advance(&state, 4));
    assert(!state.requested && state.ticks == 7);
    assert(gc_error_control_toggle(&state));
    assert(gc_error_control_advance(&state, 2));
    assert(state.requested && state.ticks == 9 && state.fade_ticks == limit);
    assert(gc_error_control_toggle(&state));
    assert(gc_error_control_advance(&state, UINT64_MAX));
    assert(!state.requested && !state.ticks && state.fade_ticks == limit);
    state = saved;
    assert(state.requested && state.ticks == 11 && state.fade_ticks == limit);
    for (unsigned tick = 11; tick < limit; ++tick)
        assert(gc_error_control_advance(&state, 1));
    assert(state.ticks == limit);
    GcErrorControl batched = saved;
    assert(gc_error_control_advance(&batched, limit - 11));
    assert(batched.requested == state.requested && batched.ticks == state.ticks &&
           batched.fade_ticks == state.fade_ticks);
}

static void test_profile_initialization(void) {
    GcErrorControl state;
    gc_error_control_init(&state, true);
    assert(gc_error_control_toggle(&state));
    assert(gc_error_control_advance(&state, 19));
    gc_error_control_init(&state, false);
    assert(!state.requested && !state.ticks && state.fade_ticks == 20);
    assert(gc_error_control_toggle(&state));
    assert(gc_error_control_advance(&state, UINT64_MAX));
    gc_error_control_init(&state, true);
    assert(!state.requested && !state.ticks && state.fade_ticks == 30);
    gc_error_control_init(NULL, true);
}

static void test_invalid_state_is_unchanged(void) {
    const GcErrorControl invalid[] = {
        {0},
        {.requested = true, .ticks = 31, .fade_ticks = 30},
        {.requested = true, .ticks = 21, .fade_ticks = 20},
        {.requested = true, .ticks = 19, .fade_ticks = 19},
        {.requested = true, .ticks = 20, .fade_ticks = 21},
        {.requested = true, .ticks = 29, .fade_ticks = 29},
        {.requested = true, .ticks = 30, .fade_ticks = 31}};
    for (size_t index = 0; index < sizeof(invalid) / sizeof(invalid[0]); ++index) {
        GcErrorControl state = invalid[index];
        assert(!gc_error_control_toggle(&state));
        assert(!gc_error_control_advance(&state, 0));
        assert(!gc_error_control_advance(&state, UINT64_MAX));
        assert(state.requested == invalid[index].requested &&
               state.ticks == invalid[index].ticks &&
               state.fade_ticks == invalid[index].fade_ticks);
        assert(gc_error_control_alpha(&state) == 0);
    }
    assert(!gc_error_control_toggle(NULL));
    assert(!gc_error_control_advance(NULL, 1));
    assert(gc_error_control_alpha(NULL) == 0);
}

int main(void) {
    for (unsigned profile = 0; profile < 2; ++profile) {
        test_fade_boundaries_and_pause(profile != 0);
        test_reversal_and_replay(profile != 0);
    }
    test_profile_initialization();
    test_invalid_state_is_unchanged();
    puts("Startup/menu error fades and reversible inspection adapter passed.");
    return 0;
}
