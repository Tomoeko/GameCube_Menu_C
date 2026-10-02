#ifndef GAMECUBE_LAYOUT_H
#define GAMECUBE_LAYOUT_H

#include "gamecube/ipl.h"
#include "gamecube/text.h"

typedef enum {
    GC_LAYOUT_MENU,
    GC_LAYOUT_CALENDAR,
    GC_LAYOUT_OPTIONS,
    GC_LAYOUT_DISC,
    GC_LAYOUT_CARD,
    GC_LAYOUT_HELP,
    GC_LAYOUT_ERROR,
    GC_LAYOUT_CALENDAR_FACE,
    GC_LAYOUT_DISC_FACE,
    GC_LAYOUT_CARD_FACE,
    GC_LAYOUT_OPTIONS_FACE,
    GC_LAYOUT_GROUP_COUNT
} GcLayoutGroup;

enum {
    GC_LAYOUT_FLIP_V = 1,
    GC_LAYOUT_FLIP_U = 2,
    GC_LAYOUT_ROTATE_UV = 4,
    GC_LAYOUT_ALIGN_BOTTOM = 1,
    GC_LAYOUT_ALIGN_TOP = 2,
    GC_LAYOUT_ALIGN_RIGHT = 4,
    GC_LAYOUT_ALIGN_LEFT = 8
};

typedef struct {
    float center_x;
    float center_y;
    float width;
    float height;
} GcLayoutBox;

typedef struct {
    float x;
    float y;
    float u;
    float v;
} GcLayoutVertex;

typedef struct {
    char name[5];
    GcLayoutBox box;
    uint16_t texture;
    uint8_t alignment;
    uint8_t flags;
} GcLayoutPane;

typedef struct {
    char name[5];
    GcLayoutBox box;
    uint32_t color_first;
    uint32_t color_second;
    uint8_t horizontal_alignment; /* Low seven bits: center=0, right=1, left=2. */
    uint8_t vertical_alignment;   /* Center=0, bottom=1, top=2. */
    int16_t letter_spacing;
    uint16_t line_spacing;
    uint16_t font_height;
    uint16_t frame_index; /* UINT16_MAX means no frame. */
    const char *embedded_text;
    size_t embedded_length;
} GcLayoutText;

typedef struct {
    char name[5];
    GcLayoutBox box;
    uint16_t parameters[10]; /* Native fields +0x0c..+0x1f, retained verbatim. */
    uint32_t colors[4];
} GcLayoutFrame;

typedef struct {
    GcLayoutVertex vertices[4];
    uint32_t colors[4]; /* TL, TR, BR, BL, packed RGBA. */
    uint16_t texture;
    bool textured;
} GcLayoutFrameQuad;

typedef struct {
    GcLayoutFrame frames[2];
    GcLayoutText rows[5];
    unsigned text_entries[5]; /* Indices in the native card STH0 table. */
    unsigned frame_count;
    unsigned row_count;
} GcLayoutCardPopup;

typedef struct {
    GcLayoutFrame frames[2];
    GcLayoutText rows[6];
    unsigned text_entries[6];
    unsigned frame_count;
    unsigned row_count;
} GcLayoutDialog;

typedef struct {
    uint16_t minimum;
    uint16_t maximum;
    uint16_t half_cycle;
} GcLayoutGlow;

typedef struct {
    float offset_x;
    float offset_y;
    uint32_t color;
} GcLayoutTextPass;

typedef struct {
    const uint8_t *bytes;
    size_t byte_count;
    size_t pane_offset;
    size_t text_offset;
    size_t frame_offset;
    unsigned pane_count;
    unsigned text_count;
    unsigned frame_count;
} GcLayoutTable;

typedef struct {
    /* Tables borrow the GcText ROM; destroy or replace that owner only afterward. */
    GcLayoutTable tables[7][GC_LAYOUT_GROUP_COUNT];
    bool europe;
} GcLayouts;

bool gc_layout_table_decode(const uint8_t *bytes, size_t byte_count,
                            GcLayoutTable *table);
bool gc_layout_pane(const GcLayoutTable *table, unsigned index, GcLayoutPane *pane);
bool gc_layout_text(const GcLayoutTable *table, unsigned index, GcLayoutText *text);
bool gc_layout_frame(const GcLayoutTable *table, unsigned index, GcLayoutFrame *frame);
/* Names are four-byte native keys. Occurrence selects repeated keys. */
bool gc_layout_find_pane(const GcLayoutTable *table, const char name[4],
                         unsigned occurrence, GcLayoutPane *pane);
bool gc_layout_find_text(const GcLayoutTable *table, const char name[4],
                         unsigned occurrence, GcLayoutText *text);
bool gc_layout_find_frame(const GcLayoutTable *table, const char name[4],
                          unsigned occurrence, GcLayoutFrame *frame);
/* TL, TR, BR, BL with native flips/rotation. All supplied IPL panes stretch on
 * both axes. Unrecovered alignment modes return false and preserve vertices. */
bool gc_layout_pane_quad(const GcLayoutPane *pane, GcLayoutVertex vertices[4]);
bool gc_layout_text_origin(const GcLayoutText *text, float measured_width,
                           float measured_height, float *x, float *y);
/* Nine native draws: gradient body, TL/TR/BL/BR corners, top/bottom/left/right
 * edge strips. Caller tint multiplies body alpha and tints textured borders. */
bool gc_layout_frame_quad(const GcLayoutFrame *frame, const GcIplImage *textures,
                          size_t texture_count, unsigned index, uint32_t tint,
                          GcLayoutFrameQuad *quad);
/* Settled action/confirmation popup. Measure all three localized action labels
 * with the native txt0 font and supply their maximum width and line height. */
bool gc_layout_card_popup(const GcLayoutTable *table, unsigned slot,
                          unsigned visible_index, bool confirmation,
                          gc_card_action action, float measured_width,
                          float measured_line_height, GcLayoutCardPopup *popup);
/* Original card selected-text foreground/glow colors, recovered at runtime
 * from the bounded card initializer in the private IPL. */
bool gc_layout_card_popup_colors(const GcText *text, uint32_t colors[2]);
bool gc_layout_glow_decode(const GcText *text, GcLayoutGlow *glow);
/* Native 25 glow draws at (-4,-2,0,2,4) pixels, then foreground pass 25.
 * ticks counts the global native scene clock; each endpoint repeats once. */
bool gc_layout_glow_pass(const GcLayoutGlow *glow, uint32_t foreground, uint32_t halo,
                         uint64_t ticks, uint8_t alpha, unsigned index,
                         GcLayoutTextPass *pass);
/* Lost-settings body plus Yes/No choices. All sizes are measured with the
 * native txt0 font; choices_width is the maximum of both choice labels. */
bool gc_layout_boot_dialog(const GcLayoutTable *table, float body_width,
                           float body_height, float choices_width,
                           float choice_line_height, GcLayoutDialog *dialog);
/* Same centered stack used by native card format dialogs, with their own
 * text row and frame selected through the original STH flags. */
bool gc_layout_confirmation_dialog(const GcLayoutTable *table,
                                   const GcLayoutText *source, float body_width,
                                   float body_height, float choices_width,
                                   float choice_line_height, GcLayoutDialog *dialog);
/* PAL boot language selector, using the native lan1 font and mes1 center. */
bool gc_layout_language_dialog(const GcLayoutTable *table, float measured_width,
                               float measured_line_height, GcLayoutDialog *dialog);
bool gc_layouts_index(const GcText *text, GcLayouts *layouts);
const GcLayoutTable *gc_layout_table(const GcLayouts *layouts, gc_language language,
                                     GcLayoutGroup group);

#endif
