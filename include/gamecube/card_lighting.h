#ifndef GAMECUBE_CARD_LIGHTING_H
#define GAMECUBE_CARD_LIGHTING_H

#include "gamecube/face_geometry.h"
#include "gamecube/layout.h"

typedef struct {
    bool selection_lights;
    uint16_t focus_ticks;
    uint16_t blink_ticks;
    uint16_t header_flags;
    uint16_t header_strength;
    float slot_height;
    float cutoff_degrees;
    float reference_brightness;
} GcCardLightingStyle;

typedef struct {
    uint16_t focus[2];
    uint16_t center;
    uint16_t blink_phase;
    bool active;
} GcCardLighting;

typedef struct {
    GcMenuGridLighting lights[3];
    uint8_t alpha[3];
} GcCardGridLighting;

/* Decode the loaded ROM's card-page profile. USA/Japan 1.0 have one fixed
 * center light; PAL 1.0 adds selection lights and header attenuation. */
bool gc_card_lighting_style_decode(const GcText *text, GcCardLightingStyle *style);
bool gc_card_lighting_advance(const GcCardLightingStyle *style, GcCardLighting *state,
                              const gc_menu *menu, uint64_t ticks);
uint8_t gc_card_lighting_header_alpha(const GcCardLightingStyle *style,
                                      const GcCardLighting *state, unsigned slot,
                                      unsigned selected_slot, uint8_t alpha);
/* Slot positions come from the midpoints of native cells 05/06 and 05/09,
 * rather than following each save's hover or its selection scale. */
bool gc_card_lighting_grid(const GcCardLightingStyle *style,
                           const GcCardLighting *state, const GcFaceGeometry *geometry,
                           const GcLayoutTable *layout, GcCardGridLighting *lighting);
bool gc_card_lighting_grid_color(const GcCardGridLighting *lighting, float x, float y,
                                 uint8_t page_alpha, float rgba[4]);

#endif
