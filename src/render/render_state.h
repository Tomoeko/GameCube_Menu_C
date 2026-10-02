#ifndef GC_RENDER_STATE_H
#define GC_RENDER_STATE_H

/* Presentation state is shared by inspection history and startup restart.
 * Resource owners and live card metadata stay outside this field list. */
#define GC_SCENE_VISUAL_FIELDS(F)                                                      \
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

#define GC_SCENE_ARRAY_FIELDS(F)                                                       \
    F(disc_face_ticks)                                                                 \
    F(disc_text_ticks)                                                                 \
    F(card_selection_ticks)                                                            \
    F(card_arrow_alpha)                                                                \
    F(card_erase_matrix)                                                               \
    F(card_erase_delays)                                                               \
    F(card_erase_angles)                                                               \
    F(card_operation_first_rows)                                                       \
    F(card_operation_centers)

#endif
