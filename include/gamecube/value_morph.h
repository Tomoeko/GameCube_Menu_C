#ifndef GAMECUBE_VALUE_MORPH_H
#define GAMECUBE_VALUE_MORPH_H

#include "gamecube/edit_geometry.h"

enum { GC_VALUE_MORPH_OFFSET_LIMIT = 1430, GC_VALUE_MORPH_NUMBER_OFFSETS = 112 };

typedef bool (*GcValueMorphRandom)(void *context, uint32_t *value);

typedef struct {
    float spread;
    float tens_multiplier;
    uint16_t duration;
    uint16_t sound_offsets;
    float disc_direction[3];
} GcValueMorphStyle;

typedef enum {
    GC_VALUE_MORPH_NONE,
    GC_VALUE_MORPH_SOUND,
    GC_VALUE_MORPH_NUMBER,
    GC_VALUE_MORPH_DISC
} GcValueMorphKind;

typedef struct {
    GcValueMorphKind kind;
    GcEditField field;
    uint8_t previous[2]; /* Sound/disc word index, or tens/units digits. */
    uint8_t next[2];
    uint16_t ticks[2];
    float direction[3];
    int16_t offsets[GC_VALUE_MORPH_OFFSET_LIMIT];
} GcValueMorphSample;

typedef struct {
    GcValueMorphSample current;
    GcValueMorphSample drawn;
    gc_page page;
    GcEditField observed_field;
    int observed_value;
    bool initialized;
    bool ready;
} GcValueMorphState;

/* Recover the regional constructor and the native random pool size. No
 * extracted tables are retained in authored source. */
bool gc_value_morph_style_decode(const GcText *text, GcValueMorphStyle *style);
void gc_value_morph_init(GcValueMorphState *state);
/* Call after the editor builder, once per video update. Sound and Disc sample
 * before detecting a change; numbers detect a change before sampling. Zero ticks
 * leave the state untouched. NULL random uses zero samples for inspection.
 * All mutable state is plain data and can be copied into rewind snapshots. */
bool gc_value_morph_advance(GcValueMorphState *state, const GcValueMorphStyle *style,
                            const gc_menu *menu, bool ready, uint64_t ticks,
                            GcValueMorphRandom random, void *context);
/* Preserve ordinary editor points, replace only the native affected word or
 * two digit columns, and append outgoing/incoming blocks in original order.
 * points may be NULL to query the count. Insufficient capacity writes nothing. */
size_t gc_value_morph_points(const GcEditGeometry *geometry, const gc_menu *menu,
                             const GcEditState *editor, const GcValueMorphStyle *style,
                             const GcValueMorphState *state, GcEditPoint *points,
                             size_t capacity);

#endif
