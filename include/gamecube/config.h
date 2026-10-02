#ifndef GAMECUBE_CONFIG_H
#define GAMECUBE_CONFIG_H

#include "gamecube/services.h"

#define GC_CONFIG_DUMMY_LIMIT 59

typedef struct {
    bool slot_present[2];
    bool dummy_data[2];
    unsigned dummy_count[2];
} GcConfig;

typedef enum {
    GC_CONFIG_OK,
    GC_CONFIG_MISSING,
    GC_CONFIG_IO,
    GC_CONFIG_INVALID
} GcConfigResult;

void gc_config_init(GcConfig *config);
/* CLI slot names a, b, or ab. Invalid input leaves mask unchanged. */
bool gc_config_noinsert_mask(const char *slots, unsigned *mask);
/* Missing files leave the defaults in place. Invalid files never partially
 * change the supplied configuration. error_line is optional and one-based.
 */
GcConfigResult gc_config_load(GcConfig *config, const char *path, size_t *error_line);
GcConfigResult gc_config_write(const GcConfig *config, const char *path);

/* Valid native64-block images with optional deterministic, independently
 * authored TEST saves and32x32 RGB5A3 icons. All payloads are generated here.
 */
gc_card_image_result gc_config_create_card(const GcConfig *config, unsigned slot,
                                           uint16_t encoding, gc_card_image *image);

/* Explicit imported images override the corresponding slot configuration.
 * Generated QA cards are seeded into caller-provided local Files shadow
 * paths at startup; absent slots leave existing files untouched. Imported
 * originals are only read by the existing service and copied into shadows.
 */
bool gc_config_prepare_services(const GcConfig *config, GcServices *services,
                                gc_menu *menu, const char *explicit_inputs[2],
                                const char *state_paths[2]);

#endif
