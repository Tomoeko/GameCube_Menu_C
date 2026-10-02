#include "gamecube/render.h"
#include "render_state.h"

#include <string.h>

bool gc_scene_reset_presentation(GcScene *scene) {
    if (!scene)
        return false;
    GcFaceGeometryState face;
    GcMenuAnimation animation;
    if (!gc_face_geometry_init(&scene->face_geometry,
                               scene->face_geometry_state.memory_arrival_delay,
                               &face) ||
        !gc_menu_animation_init(&scene->startup, &animation))
        return false;
    gc_scene_card_operation_end(scene);
#define RESET(type, field) scene->field = (type){0};
    GC_SCENE_VISUAL_FIELDS(RESET)
#undef RESET
#define RESET_ARRAY(field) memset(scene->field, 0, sizeof(scene->field));
    GC_SCENE_ARRAY_FIELDS(RESET_ARRAY)
#undef RESET_ARRAY
    if (scene->page_snapshots)
        memset(scene->page_snapshots, 0, sizeof(*scene->page_snapshots));
    memset(scene->card_operation_cards, 0, sizeof(scene->card_operation_cards));
    scene->face_geometry_state = face;
    scene->menu_animation = animation;
    scene->frame_counter = 0;
    scene->inspection_fade_alpha = 0;
    return true;
}
