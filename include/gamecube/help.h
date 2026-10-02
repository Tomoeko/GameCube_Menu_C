#ifndef GAMECUBE_HELP_H
#define GAMECUBE_HELP_H

#include "gamecube/layout.h"

enum { GC_HELP_ENTRY_LIMIT = 32, GC_HELP_PANE_DRAWS = 20 };

typedef struct {
    uint32_t backgrounds[3]; /* Stick, B, A native tints. */
    uint32_t controls[3];
    uint32_t dots;
    uint32_t text;
} GcHelpStyle;

typedef struct {
    uint8_t labels[GC_HELP_ENTRY_LIMIT]; /* Native counters, 0..20 ticks. */
    uint8_t icons[6];
    uint8_t control_intro;
    uint16_t phase;
} GcHelpState;

typedef struct {
    GcLayoutPane pane;
    GcLayoutVertex vertices[4];
    uint32_t tint;
} GcHelpPane;

bool gc_help_style_decode(const GcText *text, GcHelpStyle *style);
void gc_help_state_init(GcHelpState *state);
/* Advance at native PAL/NTSC frame rate. Scene changes preserve the counters
 * so outgoing help fades while incoming labels appear. */
bool gc_help_advance(GcHelpState *state, const gc_menu *menu, uint64_t ticks);
uint8_t gc_help_entry_alpha(const GcHelpState *state, unsigned entry);
/* Original ordered background, rotating-stick, button and dot draw calls. */
bool gc_help_pane(const GcHelpStyle *style, const GcHelpState *state,
                  const GcLayoutTable *table, unsigned index, uint8_t alpha,
                  GcHelpPane *pane);

#endif
