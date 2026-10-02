#include "gamecube/frame_history.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* Card metadata belongs to the storage revision, rather than an animation
 * frame. Its large file tables stay live across seeks; the caller resets
 * history at the same boundary that replaces those tables or their art.
 */
typedef struct {
    uint8_t before_cards[offsetof(gc_menu, cards)];
    uint8_t after_cards[sizeof(gc_menu) - offsetof(gc_menu, disc_status)];
} MenuFrame;

#define SCENE_VISUAL_FIELDS(F)                                                         \
    F(GcTextEncoding, encoding)                                                        \
    F(gc_language, language)                                                           \
    F(bool, perspective)                                                               \
    F(float, camera_y)                                                                 \
    F(float, pixel_scale_y)                                                            \
    F(float, display_offset_x)                                                         \
    F(GcFaceGeometryState, face_geometry_state)                                        \
    F(GcEditState, edit_state)                                                         \
    F(GcHelpState, help_state)                                                         \
    F(GcCardPopups, card_popups)                                                       \
    F(GcCardUsage, card_usage)                                                         \
    F(GcCardLighting, card_lighting)                                                   \
    F(GcCardCells, card_cells)                                                         \
    F(GcValueMorphState, value_morph_state)                                            \
    F(uint8_t, disc_metadata_ticks)                                                    \
    F(uint8_t, test_error_alpha)                                                       \
    F(uint8_t, fatal_error_ticks)                                                      \
    F(bool, fatal_error_latched)                                                       \
    F(uint64_t, ui_ticks)                                                              \
    F(GcPageTransitions, page_transitions)                                             \
    F(float, value_alpha)                                                              \
    F(float, grid_alpha)                                                               \
    F(float, text_alpha)                                                               \
    F(bool, help_drawn)                                                                \
    F(GcMenuAnimation, menu_animation)                                                 \
    F(GcMenuAnimationPose, menu_pose)                                                  \
    F(gc_page, animation_page)                                                         \
    F(double, animation_elapsed)                                                       \
    F(double, animation_fraction)                                                      \
    F(bool, animation_started)                                                         \
    F(uint64_t, card_ticks)                                                            \
    F(uint8_t, card_erase_tick)                                                        \
    F(bool, card_erasing)                                                              \
    F(GcCardOperation, card_operation)                                                 \
    F(GcFaceGeometryPoint, card_operation_point)                                       \
    F(bool, card_operation_active)

typedef struct {
#define DECLARE(type, field) type field;
    SCENE_VISUAL_FIELDS(DECLARE)
#undef DECLARE
    uint8_t disc_face_ticks[3];
    uint8_t disc_text_ticks[5];
    uint8_t card_selection_ticks[2][16];
    uint8_t card_arrow_alpha[2][2];
    float card_erase_matrix[12];
    uint8_t card_erase_delays[12];
    int16_t card_erase_angles[12];
    size_t card_operation_first_rows[2];
    float card_operation_centers[2][16][2];
    MenuFrame pages[GC_TRANSITION_GROUP_COUNT];
    GcEditState page_editors[GC_TRANSITION_GROUP_COUNT];
    GcValueMorphState page_morphs[GC_TRANSITION_GROUP_COUNT];
    bool page_saved[GC_TRANSITION_GROUP_COUNT];
} SceneFrame;

typedef struct {
    GcCardOperation operation;
    unsigned frame_rate;
    double fraction;
    int32_t adapter_result;
    uint8_t metadata_pending;
    unsigned destination_file_index;
    bool active;
    bool random_pending;
} CardFrame;

typedef struct {
    uint64_t counter;
    MenuFrame menu;
    GcBootControl boot;
    SceneFrame scene;
    GcInputControl input;
    GcBootInput boot_input;
    GcFaceRandom random;
    CardFrame cards;
    GcLaunchControl launch;
    GcDiscControl disc;
    GcErrorControl error;
    bool pending_disc_toggle;
    bool disc_toggle_held;
    bool pending_error_toggle;
    bool error_toggle_held;
    bool launching;
    gc_menu *launch_menu;
    double boot_fraction;
    double input_fraction;
    double launch_fraction;
    bool startup_waiting;
    uint64_t startup_wait_ticks;
    double startup_delay_elapsed;
    MenuFrame launch_menu_frame;
    bool has_runtime;
} Frame;

struct GcFrameHistory {
    Frame *frames;
    size_t capacity;
    size_t first;
    size_t count;
    size_t position;
};

static void save_menu(MenuFrame *frame, const gc_menu *menu) {
    memcpy(frame->before_cards, menu, sizeof(frame->before_cards));
    memcpy(frame->after_cards, (const uint8_t *)menu + offsetof(gc_menu, disc_status),
           sizeof(frame->after_cards));
}

static void restore_menu(const MenuFrame *frame, gc_menu *menu) {
    memcpy(menu, frame->before_cards, sizeof(frame->before_cards));
    memcpy((uint8_t *)menu + offsetof(gc_menu, disc_status), frame->after_cards,
           sizeof(frame->after_cards));
}

#define SCENE_ARRAY_FIELDS(F)                                                          \
    F(disc_face_ticks)                                                                 \
    F(disc_text_ticks)                                                                 \
    F(card_selection_ticks)                                                            \
    F(card_arrow_alpha)                                                                \
    F(card_erase_matrix)                                                               \
    F(card_erase_delays)                                                               \
    F(card_erase_angles)                                                               \
    F(card_operation_first_rows)                                                       \
    F(card_operation_centers)

static void save_scene(SceneFrame *frame, const GcScene *scene) {
#define SAVE(type, field) memcpy(&frame->field, &scene->field, sizeof(frame->field));
    SCENE_VISUAL_FIELDS(SAVE)
#undef SAVE
#define SAVE_ARRAY(field) memcpy(frame->field, scene->field, sizeof(frame->field));
    SCENE_ARRAY_FIELDS(SAVE_ARRAY)
#undef SAVE_ARRAY
    memset(frame->page_saved, 0, sizeof(frame->page_saved));
    if (scene->page_snapshots) {
        for (unsigned page = 0; page < GC_TRANSITION_GROUP_COUNT; ++page) {
            frame->page_saved[page] = scene->page_snapshots->saved[page];
            save_menu(&frame->pages[page], &scene->page_snapshots->menus[page]);
            frame->page_editors[page] = scene->page_snapshots->editors[page];
            frame->page_morphs[page] = scene->page_snapshots->morphs[page];
        }
    }
}

static void restore_scene(const SceneFrame *frame, GcScene *scene) {
#define RESTORE(type, field) memcpy(&scene->field, &frame->field, sizeof(frame->field));
    SCENE_VISUAL_FIELDS(RESTORE)
#undef RESTORE
#define RESTORE_ARRAY(field) memcpy(scene->field, frame->field, sizeof(frame->field));
    SCENE_ARRAY_FIELDS(RESTORE_ARRAY)
#undef RESTORE_ARRAY
    if (scene->page_snapshots) {
        for (unsigned page = 0; page < GC_TRANSITION_GROUP_COUNT; ++page) {
            scene->page_snapshots->saved[page] = frame->page_saved[page];
            restore_menu(&frame->pages[page], &scene->page_snapshots->menus[page]);
            scene->page_snapshots->editors[page] = frame->page_editors[page];
            scene->page_snapshots->morphs[page] = frame->page_morphs[page];
        }
    }
}

GcFrameHistory *gc_frame_history_create(size_t capacity) {
    if (capacity < 2 || capacity > SIZE_MAX / sizeof(Frame))
        return NULL;
    GcFrameHistory *history = calloc(1, sizeof(*history));
    if (!history)
        return NULL;
    history->frames = calloc(capacity, sizeof(*history->frames));
    if (!history->frames) {
        free(history);
        return NULL;
    }
    history->capacity = capacity;
    return history;
}

void gc_frame_history_destroy(GcFrameHistory *history) {
    if (!history)
        return;
    free(history->frames);
    free(history);
}

void gc_frame_history_reset(GcFrameHistory *history) {
    if (history)
        history->first = history->count = history->position = 0;
}

bool gc_frame_history_save(GcFrameHistory *history, uint64_t counter,
                           const gc_menu *menu, const GcBootControl *boot,
                           const GcScene *scene, const GcFrameRuntime *runtime) {
    if (!history || !menu || !boot || !scene)
        return false;
    if (history->count)
        history->count = history->position + 1;
    if (history->count == history->capacity) {
        history->first = (history->first + 1) % history->capacity;
        --history->count;
    }
    size_t slot = (history->first + history->count) % history->capacity;
    Frame *frame = &history->frames[slot];
    frame->counter = counter;
    save_menu(&frame->menu, menu);
    frame->boot = *boot;
    save_scene(&frame->scene, scene);
    frame->has_runtime = runtime != NULL;
    if (runtime) {
        frame->input = runtime->input;
        frame->boot_input = runtime->boot_input;
        frame->random = runtime->random;
        frame->launch = runtime->launch;
        frame->disc = runtime->disc;
        frame->error = runtime->error;
        frame->pending_disc_toggle = runtime->pending_disc_toggle;
        frame->disc_toggle_held = runtime->disc_toggle_held;
        frame->pending_error_toggle = runtime->pending_error_toggle;
        frame->error_toggle_held = runtime->error_toggle_held;
        frame->launching = runtime->launching;
        frame->launch_menu = runtime->launch_menu;
        frame->boot_fraction = runtime->boot_fraction;
        frame->input_fraction = runtime->input_fraction;
        frame->launch_fraction = runtime->launch_fraction;
        frame->startup_waiting = runtime->startup_waiting;
        frame->startup_wait_ticks = runtime->startup_wait_ticks;
        frame->startup_delay_elapsed = runtime->startup_delay_elapsed;
        if (frame->launch_menu)
            save_menu(&frame->launch_menu_frame, frame->launch_menu);
        const GcCardRuntime *cards = &runtime->cards;
        frame->cards =
            (CardFrame){cards->operation,        cards->frame_rate,
                        cards->fraction,         cards->adapter_result,
                        cards->metadata_pending, cards->destination_file_index,
                        cards->active,           cards->random_pending};
    }
    history->position = history->count++;
    return true;
}

bool gc_frame_history_seek(GcFrameHistory *history, int delta, uint64_t *counter,
                           gc_menu *menu, GcBootControl *boot, GcScene *scene,
                           GcFrameRuntime *runtime) {
    if (!history || !history->count || !counter || !menu || !boot || !scene)
        return false;
    size_t amount = delta < 0 ? (size_t)(-(int64_t)delta) : (size_t)delta;
    if (delta < 0 ? amount > history->position
                  : amount >= history->count - history->position)
        return false;
    size_t position =
        delta < 0 ? history->position - amount : history->position + amount;
    const Frame *frame =
        &history->frames[(history->first + position) % history->capacity];
    *counter = frame->counter;
    restore_menu(&frame->menu, menu);
    *boot = frame->boot;
    restore_scene(&frame->scene, scene);
    if (runtime && frame->has_runtime) {
        runtime->input = frame->input;
        runtime->boot_input = frame->boot_input;
        runtime->random = frame->random;
        runtime->launch = frame->launch;
        runtime->disc = frame->disc;
        runtime->error = frame->error;
        runtime->pending_disc_toggle = frame->pending_disc_toggle;
        runtime->disc_toggle_held = frame->disc_toggle_held;
        runtime->pending_error_toggle = frame->pending_error_toggle;
        runtime->error_toggle_held = frame->error_toggle_held;
        runtime->launching = frame->launching;
        runtime->launch_menu = frame->launch_menu;
        runtime->boot_fraction = frame->boot_fraction;
        runtime->input_fraction = frame->input_fraction;
        runtime->launch_fraction = frame->launch_fraction;
        runtime->startup_waiting = frame->startup_waiting;
        runtime->startup_wait_ticks = frame->startup_wait_ticks;
        runtime->startup_delay_elapsed = frame->startup_delay_elapsed;
        if (runtime->launch_menu)
            restore_menu(&frame->launch_menu_frame, runtime->launch_menu);
        GcCardRuntime *cards = &runtime->cards;
        const CardFrame *saved = &frame->cards;
        cards->operation = saved->operation;
        cards->frame_rate = saved->frame_rate;
        cards->fraction = saved->fraction;
        cards->adapter_result = saved->adapter_result;
        cards->metadata_pending = saved->metadata_pending;
        cards->destination_file_index = saved->destination_file_index;
        cards->active = saved->active;
        cards->random_pending = saved->random_pending;
    }
    history->position = position;
    return true;
}

size_t gc_frame_history_count(const GcFrameHistory *history) {
    return history ? history->count : 0;
}

size_t gc_frame_history_position(const GcFrameHistory *history) {
    return history ? history->position : 0;
}
