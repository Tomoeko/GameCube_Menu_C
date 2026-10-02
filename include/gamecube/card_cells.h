#ifndef GAMECUBE_CARD_CELLS_H
#define GAMECUBE_CARD_CELLS_H

#include "gamecube/text.h"

typedef bool (*GcCardCellsRandom)(void *context, uint32_t *value);

typedef struct {
    float origin[3];
    float tangent_radius;
    float offscreen_distance;
    uint16_t entrance_ticks;
    uint16_t relayout_ticks;
    uint8_t delay_mask;
    uint8_t entrance_alpha_step;
    uint8_t alpha_step;
} GcCardCellStyle;

typedef struct {
    float position[3];
    float velocity[3];
    float start[3];
    float tangent[3];
    float target[3];
    float drawn_position[3];
    uint16_t counter;
    uint16_t duration;
    uint8_t delay;
    uint8_t alpha;
    uint8_t target_alpha;
    uint8_t selection;
    uint8_t drawn_alpha;
    uint8_t drawn_selection;
} GcCardCell;

typedef struct {
    GcCardCell cells[2][GC_CARD_FILE_LIMIT];
    size_t first_rows[2];
    uint16_t phase;
    uint16_t drawn_phase;
    bool initialized;
    bool entrance;
} GcCardCells;

/* All coordinates in the motion records are native fixed12.4 pixel units.
 * The supplied centers are logical GUI pixels, read from the original GLH.
 * A NULL random adapter uses zero samples for reproducible inspection. */
bool gc_card_cell_style_decode(const GcText *text, GcCardCellStyle *style);
bool gc_card_cells_begin(const GcCardCellStyle *style, GcCardCells *state,
                         const float centers[2][16][2], const size_t first_rows[2],
                         GcCardCellsRandom random, void *context);
bool gc_card_cells_relayout(const GcCardCellStyle *style, GcCardCells *state,
                            const float centers[2][16][2], const size_t first_rows[2]);
/* The original draws each record before updating it. Sample drawn_position,
 * drawn_alpha and drawn_selection after advancing once per video update.
 * The face-collapse gate freezes both motion and alpha. Entrance completes
 * only when all READY cards and the three page faders are ready. */
bool gc_card_cells_advance(const GcCardCellStyle *style, GcCardCells *state,
                           const bool ready_cards[2], bool face_ready, bool page_ready,
                           bool erasing, unsigned selected_slot, size_t selected_index,
                           uint64_t ticks);
bool gc_card_cells_ready(const GcCardCells *state, const bool ready_cards[2]);

#endif
