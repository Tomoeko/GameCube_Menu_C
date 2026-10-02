#include "gamecube/card_cells.h"
#include "gamecube/layout.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static const GcCardCellStyle style = {{4672, 3584, 0}, 350, 512, 60, 40, 15, 25, 8};

static void make_centers(float centers[2][16][2]) {
    for (unsigned slot = 0; slot < 2; ++slot)
        for (unsigned cell = 0; cell < 16; ++cell) {
            centers[slot][cell][0] = (float)(84 + slot * 260 + cell % 4 * 56);
            centers[slot][cell][1] = (float)(106 + cell / 4 * 56);
        }
}

typedef struct {
    unsigned count;
    unsigned fail_at;
} Samples;

static bool random_sample(void *context, uint32_t *value) {
    Samples *samples = context;
    if (samples->count == samples->fail_at)
        return false;
    *value = samples->count++ % 2 ? 15 : 0x4000;
    return true;
}

static void test_delay_collapse_and_readiness(void) {
    float centers[2][16][2];
    const size_t rows[2] = {0, 0};
    const bool ready[2] = {true, true};
    GcCardCells state = {0};
    Samples samples = {0, 1000};
    make_centers(centers);
    assert(gc_card_cells_begin(&style, &state, centers, rows, random_sample, &samples));
    assert(samples.count == 508);
    GcCardCell *cell = &state.cells[0][0];
    assert(cell->delay == 15 && cell->duration == 75);
    assert(fabsf(cell->tangent[0] - 350) < 0.01f);
    assert(fabsf(cell->tangent[1]) < 0.001f);
    assert(cell->position[0] == 4672 && cell->alpha == 0 && state.entrance);
    GcCardCells frozen = state;
    assert(gc_card_cells_advance(&style, &state, ready, false, true, false, 0, 0, 100));
    assert(memcmp(&frozen, &state, sizeof(state)) == 0);
    assert(gc_card_cells_advance(&style, &state, ready, true, false, false, 0, 0, 15));
    assert(cell->counter == 15 && cell->alpha == 0 && cell->position[0] == 4672);
    assert(gc_card_cells_advance(&style, &state, ready, true, false, false, 0, 0, 1));
    assert(cell->counter == 16 && cell->alpha == 25 && cell->drawn_alpha == 0);
    assert(cell->position[0] == 4672);
    assert(gc_card_cells_advance(&style, &state, ready, true, false, false, 0, 0, 60));
    assert(gc_card_cells_ready(&state, ready));
    assert(state.entrance && cell->selection == 0);
    assert(fabsf(cell->position[0] - centers[0][0][0] * 16) < 0.01f);
    assert(gc_card_cells_advance(&style, &state, ready, true, true, true, 0, 0, 1));
    assert(state.entrance);
    assert(gc_card_cells_advance(&style, &state, ready, true, true, false, 0, 0, 1));
    assert(!state.entrance && cell->counter == 40 && cell->duration == 40);
    assert(gc_card_cells_advance(&style, &state, ready, true, true, false, 0, 0, 6));
    assert(cell->selection == 6 && cell->drawn_selection == 5);
    GcCardCells unchanged = state;
    assert(gc_card_cells_advance(&style, &state, ready, true, true, false, 0, 0, 0));
    assert(memcmp(&unchanged, &state, sizeof(state)) == 0);
}

static void test_scroll_and_atomic_samples(void) {
    float centers[2][16][2];
    const size_t first[2] = {0, 0};
    const size_t scrolled[2] = {1, 0};
    const bool ready[2] = {true, false};
    GcCardCells state = {0};
    make_centers(centers);
    assert(gc_card_cells_begin(&style, &state, centers, first, NULL, NULL));
    GcCardCells before = state;
    Samples samples = {0, 3};
    assert(
        !gc_card_cells_begin(&style, &state, centers, first, random_sample, &samples));
    assert(memcmp(&before, &state, sizeof(state)) == 0);
    assert(gc_card_cells_advance(&style, &state, ready, true, true, false, 0, 0, 100));
    assert(!state.entrance);
    assert(gc_card_cells_relayout(&style, &state, centers, scrolled));
    assert(state.cells[0][0].target_alpha == 0);
    assert(state.cells[0][4].target_alpha == 255);
    assert(state.cells[0][19].target_alpha == 255);
    assert(state.cells[0][20].target_alpha == 0);
    assert(state.cells[0][0].target[1] == centers[0][0][1] * 16 - 512);
    assert(!gc_card_cells_ready(&state, ready));
    assert(gc_card_cells_advance(&style, &state, ready, true, true, false, 0, 4, 20));
    assert(state.cells[0][0].alpha == 95);
    assert(state.cells[0][16].alpha == 160);
    assert(gc_card_cells_advance(&style, &state, ready, true, true, false, 0, 4,
                                 UINT64_MAX));
    assert(gc_card_cells_ready(&state, ready));
    assert(state.cells[0][0].alpha == 0 && state.cells[0][16].alpha == 255);
    assert(fabsf(state.cells[0][4].position[1] - centers[0][0][1] * 16) < 0.01f);
    assert(state.cells[0][4].selection == 6);
}

static void test_native(const char *path) {
    GcText text = {0};
    GcLayouts layouts;
    GcCardCellStyle recovered;
    assert(gc_text_load(path, &text));
    assert(gc_card_cell_style_decode(&text, &recovered));
    assert(recovered.entrance_ticks == (text.europe ? 33 : 60));
    assert(recovered.relayout_ticks == 40 && recovered.tangent_radius == 350);
    assert(recovered.offscreen_distance == 512 && recovered.origin[0] == 4672);
    assert(recovered.origin[1] == 3584 && recovered.origin[2] == 0);
    assert(recovered.entrance_alpha_step == 25 && recovered.alpha_step == 8);
    assert(gc_layouts_index(&text, &layouts));
    const GcLayoutTable *table =
        gc_layout_table(&layouts, GC_LANGUAGE_ENGLISH, GC_LAYOUT_CARD);
    float centers[2][16][2];
    for (unsigned slot = 0; slot < 2; ++slot)
        for (unsigned cell = 0; cell < 16; ++cell) {
            char name[5];
            GcLayoutPane pane;
            snprintf(name, sizeof(name), "i%c%02u", slot ? 'b' : 'a', cell);
            assert(gc_layout_find_pane(table, name, 0, &pane));
            centers[slot][cell][0] = pane.box.center_x;
            centers[slot][cell][1] = pane.box.center_y;
        }
    GcCardCells state = {0};
    const size_t rows[2] = {0, 0};
    const bool ready[2] = {true, true};
    assert(gc_card_cells_begin(&recovered, &state, centers, rows, NULL, NULL));
    assert(gc_card_cells_advance(&recovered, &state, ready, true, true, false, 0, 0,
                                 recovered.entrance_ticks + 2));
    assert(!state.entrance && gc_card_cells_ready(&state, ready));
    assert(fabsf(state.cells[1][15].position[0] - centers[1][15][0] * 16) < 0.01f);
    GcCardCellStyle unchanged = recovered;
    size_t offset = text.europe ? 0x210fc : 0x20024;
    text.rom[offset] ^= 1;
    assert(!gc_card_cell_style_decode(&text, &recovered));
    assert(memcmp(&unchanged, &recovered, sizeof(recovered)) == 0);
    gc_text_destroy(&text);
}

int main(int argc, char **argv) {
    test_delay_collapse_and_readiness();
    test_scroll_and_atomic_samples();
    if (argc > 1)
        test_native(argv[1]);
    puts("card cell tests passed");
    return 0;
}
