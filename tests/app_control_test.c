#define _POSIX_C_SOURCE 200809L
#include "gamecube/render.h"
#include "software.h"

#include <assert.h>
#include <stdio.h>
#include <time.h>

static int test_clock(clockid_t clock_id, struct timespec *value);
static bool test_poll(CcPlatform *platform, CcEvent *event);
static void test_draw(GcScene *scene, const gc_menu *menu);

/* Mock only the host clock/event boundary. The actual CLI, configuration,
 * native controllers, resource decoder and scene renderer remain active.
 */
#define clock_gettime test_clock
#define cc_platform_poll test_poll
#define gc_scene_draw test_draw
#define main test_application_main
#include "../src/main.c"
#undef main
#undef gc_scene_draw
#undef cc_platform_poll
#undef clock_gettime

static unsigned event_phase;
static unsigned absent_mask;
static uint64_t drawn_counter;
static unsigned drawn_frames;
static gc_region expected_region;
static bool test_disc_events;
static uint32_t initial_disc_texture;
static bool clock_failure;

static bool poll_disc_events(CcEvent *event) {
    static const CcKey keys[] = {'d', 'd', '.', '.', 'd',           CC_KEY_UNKNOWN,
                                 'D', '.', '.', 'D', CC_KEY_UNKNOWN};
    unsigned phase = event_phase++;
    if (phase == sizeof(keys) / sizeof(keys[0])) {
        assert(drawn_counter == 2 && drawn_frames == 3);
        event->type = CC_EVENT_QUIT;
        return true;
    }
    if (phase >= sizeof(keys) / sizeof(keys[0]))
        return false;
    if (keys[phase] == CC_KEY_UNKNOWN)
        return false;
    bool up = phase == 3 || phase == 4 || phase == 8 || phase == 9;
    *event =
        (CcEvent){.type = up ? CC_EVENT_KEY_UP : CC_EVENT_KEY_DOWN, .key = keys[phase]};
    return true;
}

static int test_clock(clockid_t clock_id, struct timespec *value) {
    (void)clock_id;
    if (clock_failure)
        return -1;
    *value = (struct timespec){.tv_sec = event_phase >= 3 ? 1 : 0};
    return 0;
}

static bool test_poll(CcPlatform *platform, CcEvent *event) {
    (void)platform;
    *event = (CcEvent){0};
    if (test_disc_events)
        return poll_disc_events(event);
    switch (event_phase++) {
        case 0:
        case 3:
            if (event_phase == 4) {
                /* A full second of render backlog must yield to the next
                 * host poll after ONE frame, so Space can pause promptly. */
                assert(drawn_counter == 1 && drawn_frames == 2);
            }
            *event = (CcEvent){.type = CC_EVENT_KEY_DOWN, .key = (CcKey)' '};
            return true;
        case 1:
        case 4:
            *event = (CcEvent){.type = CC_EVENT_KEY_UP, .key = (CcKey)' '};
            return true;
        case 2:
        case 5:
            return false;
        case 6:
            assert(drawn_counter == 1 && drawn_frames == 2);
            event->type = CC_EVENT_QUIT;
            return true;
        default:
            return false;
    }
}

static void test_draw(GcScene *scene, const gc_menu *menu) {
    assert(scene->frame_counter_enabled);
    assert(menu->region == expected_region);
    if (expected_region == GC_REGION_USA)
        assert(menu->settings.language == GC_LANGUAGE_ENGLISH);
    drawn_counter = scene->frame_counter;
    ++drawn_frames;
    if (test_disc_events) {
        gc_disc_status expected_disc =
            drawn_counter ? GC_DISC_LID_OPEN : GC_DISC_ABSENT;
        assert(menu->disc_status == expected_disc);
        assert(scene->disc && scene->disc->simulated && scene->disc_banner);
        if (!drawn_counter)
            initial_disc_texture = scene->disc_banner;
        else
            assert(scene->disc_banner == initial_disc_texture);
    }
    for (unsigned slot = 0; slot < 2; ++slot)
        assert(menu->cards[slot].status ==
               (absent_mask & (1u << slot) ? GC_CARD_ABSENT : GC_CARD_READY));
    gc_scene_draw(scene, menu);
}

int main(int argc, char **argv) {
    if (argc < 3)
        return EXIT_SUCCESS;
    GcConfig config;
    gc_config_init(&config);
    config.dummy_data[0] = config.dummy_data[1] = true;
    assert(gc_config_write(&config, "Files/config.ini") == GC_CONFIG_OK);
    gc_card_image original = {0};
    assert(gc_config_create_card(&config, 0, 0, &original) == GC_CARD_IMAGE_OK);
    assert(gc_card_image_write(&original, "Files/import.raw") == GC_CARD_IMAGE_OK);
    const char *slots[] = {"a", "b", "ab"};
    expected_region = !strcmp(argv[2], "EUR")   ? GC_REGION_EUROPE
                      : !strcmp(argv[2], "JAP") ? GC_REGION_JAPAN
                                                : GC_REGION_USA;
    for (unsigned index = 0; index < 3; ++index) {
        assert(gc_config_noinsert_mask(slots[index], &absent_mask));
        event_phase = drawn_frames = 0;
        drawn_counter = 0;
        char *arguments[] = {"gamecube-menu", "--ipl",           argv[1],
                             "--region",      argv[2],           "--skip-startup",
                             "--step",        "--noinsert",      (char *)slots[index],
                             "--card-a",      "Files/import.raw"};
        assert(test_application_main(11, arguments) == EXIT_SUCCESS);
        assert(drawn_counter == 1 && drawn_frames == 2);
    }
    if (expected_region == GC_REGION_USA) {
        absent_mask = 0;
        event_phase = drawn_frames = 0;
        drawn_counter = 0;
        char *arguments[] = {"gamecube-menu", "--ipl", argv[1], "--skip-startup",
                             "--step"};
        assert(test_application_main(5, arguments) == EXIT_SUCCESS);
        assert(drawn_counter == 1 && drawn_frames == 2);
    }
    test_disc_events = true;
    absent_mask = 0;
    event_phase = drawn_frames = 0;
    drawn_counter = 0;
    char *disc_arguments[] = {"gamecube-menu", "--ipl",          argv[1], "--region",
                              argv[2],         "--skip-startup", "--step"};
    assert(test_application_main(7, disc_arguments) == EXIT_SUCCESS);
    assert(drawn_counter == 2 && drawn_frames == 3);
    clock_failure = true;
    drawn_frames = 0;
    assert(test_application_main(7, disc_arguments) == EXIT_FAILURE);
    assert(drawn_frames == 0);
    clock_failure = false;
    gc_card_image imported = {0};
    assert(gc_card_image_load(&imported, "Files/import.raw") == GC_CARD_IMAGE_OK);
    assert(imported.byte_count == original.byte_count &&
           !memcmp(imported.bytes, original.bytes, original.byte_count));
    gc_card_image_free(&imported);
    gc_card_image_free(&original);
    puts("Actual CLI card overrides and host-event responsiveness passed.");
    return EXIT_SUCCESS;
}
