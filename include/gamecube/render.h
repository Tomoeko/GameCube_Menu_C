#ifndef GAMECUBE_RENDER_H
#define GAMECUBE_RENDER_H

#include "gamecube/ipl.h"
#include "gamecube/card_image.h"
#include "gamecube/menu.h"
#include "gamecube/font.h"
#include "gamecube/text.h"
#include "gamecube/disc.h"
#include "gamecube/startup.h"
#include "gamecube/ipl_animation.h"
#include "gamecube/layout.h"
#include "gamecube/menu_animation.h"
#include "gamecube/face_geometry.h"
#include "gamecube/edit_geometry.h"
#include "gamecube/boot_control.h"
#include "gamecube/help.h"
#include "gamecube/page_transition.h"
#include "gamecube/card_operation.h"
#include "gamecube/card_popup.h"
#include "gamecube/card_usage.h"
#include "gamecube/card_lighting.h"
#include "gamecube/card_cells.h"
#include "gamecube/value_morph.h"
#include "console_common/platform/platform.h"

typedef struct GcMesh GcMesh;
typedef struct GcCardTextures GcCardTextures;
typedef struct GcUiTextures GcUiTextures;
typedef struct {
    gc_menu menus[GC_TRANSITION_GROUP_COUNT];
    GcEditState editors[GC_TRANSITION_GROUP_COUNT];
    GcValueMorphState morphs[GC_TRANSITION_GROUP_COUNT];
    bool saved[GC_TRANSITION_GROUP_COUNT];
} GcPageSnapshots;

typedef struct {
    CcPlatform *platform; /* Borrowed; outlives this scene. */
    GcFont font;
    GcText native_text;
    GcLayouts layouts;
    GcUiTextures *ui;
    GcTextEncoding encoding;
    gc_language language;
    bool perspective;
    float camera_y;
    float pixel_scale_y;
    float display_offset_x;
    uint32_t font_texture;
    uint32_t sjis_texture;
    uint32_t disc_banner;
    const GcDisc *disc; /* Borrowed, including localized banner metadata. */
    GcMesh *menu_cube;
    GcMesh *boot_mark;
    GcMesh *boot_base;
    GcMesh *boot_cover;
    GcMesh *moving_cube;
    GcMesh *logotype;
    GcMesh *face_cube;
    GcMesh *glyph_cube;
    GcMesh *card_base;
    GcMesh *card_cover;
    GcFaceGeometry face_geometry;
    GcFaceGeometryState face_geometry_state;
    GcEditGeometry edit_geometry;
    GcEditState edit_state;
    GcValueMorphStyle value_morph_style;
    GcValueMorphState value_morph_state;
    GcHelpStyle help_style;
    GcHelpState help_state;
    GcLayoutGlow text_glow;
    uint32_t popup_colors[2];
    GcCardPopupStyle popup_style;
    GcCardPopups card_popups;
    GcCardUsage card_usage;
    GcCardLightingStyle card_lighting_style;
    GcCardLighting card_lighting;
    GcCardCellStyle card_cell_style;
    GcCardCells card_cells;
    GcCardCellsRandom card_random;
    void *card_random_context;
    uint64_t ui_ticks;
    uint8_t disc_face_ticks[3]; /* Banner, absent-disc, questionmark faders. */
    uint8_t disc_metadata_ticks;
    uint8_t test_error_alpha;
    uint8_t fatal_error_ticks;
    bool fatal_error_latched;
    uint8_t disc_text_ticks[5];
    GcPageTransitionStyle page_style;
    GcPageTransitions page_transitions;
    GcPageSnapshots *page_snapshots;
    float value_alpha;
    float grid_alpha;
    float text_alpha;
    bool help_drawn;
    GcStartup startup;
    const GcBootConfig *boot_config;
    const GcBootControl *boot_control;
    GcMenuAnimation menu_animation;
    GcMenuAnimationPose menu_pose;
    gc_page animation_page;
    double animation_elapsed;
    double animation_fraction;
    bool animation_started;
    uint8_t card_selection_ticks[2][16];
    uint8_t card_arrow_alpha[2][2];
    uint64_t card_ticks;
    float card_erase_matrix[12];
    uint8_t card_erase_delays[12];
    int16_t card_erase_angles[12];
    uint8_t card_erase_tick;
    bool card_erasing;
    GcCardOperation card_operation;
    gc_card card_operation_cards[2];
    size_t card_operation_first_rows[2];
    float card_operation_centers[2][16][2];
    GcFaceGeometryPoint card_operation_point;
    GcCardTextures *card_operation_art[2];
    bool card_operation_active;
    GcIplAnimation logotype_joints;
    GcIplAnimation logotype_colors;
    uint32_t trail_texture;
    GcCardTextures *card_art[2];
    bool frame_counter_enabled;
    uint64_t frame_counter;
    uint8_t inspection_fade_alpha;
    unsigned volume_indicator_percent; /* Host overlay; outside frame history. */
    float volume_indicator_alpha;
} GcScene;

bool gc_scene_init(GcScene *scene, CcPlatform *platform, const char *ipl_path);
void gc_scene_destroy(GcScene *scene);
/* Restart presentation counters while retaining decoded resources, artwork,
 * and host display preferences. No resource decoding or GPU upload occurs. */
bool gc_scene_reset_presentation(GcScene *scene);
bool gc_scene_set_cards(GcScene *scene, const gc_card_image cards[2]);
bool gc_scene_set_disc(GcScene *scene, const GcDisc *disc);
/* Borrow the recovered random stream for future full-card entrances. */
void gc_scene_set_card_random(GcScene *scene, GcCardCellsRandom adapter, void *context);
/* Capture the selected card's native transform before its cell disappears.
 * Delays and angles are samples from the caller's recovered random streams. */
bool gc_scene_card_erase(GcScene *scene, unsigned slot, unsigned visible_cell,
                         const uint8_t delays[12], const int16_t angles[12]);
/* Local card operations retain private art and native geometry until their
 * recovered controller permits dismissal. The snapshots own GPU textures.
 */
bool gc_scene_card_operation_begin(GcScene *scene, const gc_menu *menu,
                                   const GcCardOperation *operation,
                                   const int16_t piece_angles[12]);
void gc_scene_card_operation_update(GcScene *scene, const GcCardOperation *operation);
/* Apply the metadata change before the native forty-tick cell relayout. */
bool gc_scene_card_relayout(GcScene *scene, const gc_menu *menu,
                            const GcCardOperation *operation);
bool gc_scene_card_operation_ready(const GcScene *scene);
void gc_scene_card_operation_end(GcScene *scene);
/* Borrow the live dispatcher; NULL restores deterministic normal capture. */
void gc_scene_set_boot(GcScene *scene, const GcBootConfig *config,
                       const GcBootControl *control);
/* Native input gates use the current target, not a previously drawn pose.
 * Call before passing a controller edge/repeat to the menu services. */
bool gc_scene_can_press(const GcScene *scene, const gc_menu *menu, gc_button button);
/* Inspection overlay uses the original font after the inspection fader. */
void gc_scene_frame_counter(GcScene *scene, uint64_t counter);
/* Host menu-volume overlay; does not advance or rewind scene animation. */
void gc_scene_volume_indicator(GcScene *scene, unsigned percent, float alpha);
/* Clear an inspection/start-delay frame without advancing visual counters. */
void gc_scene_draw_wait(GcScene *scene);
void gc_scene_draw(GcScene *scene, const gc_menu *menu);
/* Refresh host overlays at the last native pose without updating controllers. */
void gc_scene_redraw(GcScene *scene, const gc_menu *menu);

#endif
