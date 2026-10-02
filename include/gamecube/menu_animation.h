#ifndef GAMECUBE_MENU_ANIMATION_H
#define GAMECUBE_MENU_ANIMATION_H

#include "gamecube/menu.h"
#include "gamecube/startup.h"

#define GC_MENU_ANIMATION_PANES 5
#define GC_MENU_ANIMATION_HOME_PANE 4

typedef struct {
    int16_t angles[2];
    uint16_t oscillator_phase;
    uint16_t focus_angle;
} GcMenuAnimation;

typedef struct {
    float cube_matrix[12];
    float cube_scale;
    uint8_t glass_alpha;
    float pane_matrices[GC_MENU_ANIMATION_PANES][12];
    uint8_t pane_alpha[GC_MENU_ANIMATION_PANES];
    uint8_t frame_alpha[GC_MENU_ANIMATION_PANES];
    bool rotation_complete;
    bool focus_complete;
} GcMenuAnimationPose;

/* The configuration borrows the recovered native menu profile in GcStartup.
 * Initialize once when normal boot reaches the menu. Advance exactly once
 * per regional video tick, retaining state across selection changes.
 */
bool gc_menu_animation_init(const GcStartup *startup, GcMenuAnimation *animation);
bool gc_menu_animation_update(const GcStartup *startup, GcMenuAnimation *animation,
                              bool face_selected, gc_face face, bool editing);

/* cube_matrix transforms model vertices before the native camera. Its scale
 * is separate because the original applies 1.1 to the model's root joint.
 * Pane matrices accept native local GLH coordinates without extra Y flips.
 * glass_alpha fades the glass and selected-side panes during page focus.
 * pane_alpha is the supplied color alpha; frame_alpha includes the native
 * second alpha multiplication used by pane frames and most labels.
 */
bool gc_menu_animation_sample(const GcStartup *startup,
                              const GcMenuAnimation *animation, bool face_selected,
                              gc_face face, bool editing, GcMenuAnimationPose *pose);

#endif
