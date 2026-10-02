#include "gamecube/render.h"
#include "render/software/software.h"
#include "support/output.h"
#include "support/option_values.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool set_page(gc_menu *menu, const char *name) {
    if (strcmp(name, "startup") == 0) {
        menu->page = GC_PAGE_STARTUP;
    } else if (strcmp(name, "cube") == 0) {
        menu->page = GC_PAGE_CUBE;
    } else if (strcmp(name, "calendar-face") == 0) {
        menu->page = GC_PAGE_FACE;
        menu->face = GC_FACE_CALENDAR;
    } else if (strcmp(name, "options-face") == 0) {
        menu->page = GC_PAGE_FACE;
        menu->face = GC_FACE_OPTIONS;
    } else if (strcmp(name, "cards-face") == 0) {
        menu->page = GC_PAGE_FACE;
        menu->face = GC_FACE_MEMORY_CARD;
    } else if (strcmp(name, "disc-face") == 0) {
        menu->page = GC_PAGE_FACE;
        menu->face = GC_FACE_GAME_PLAY;
    } else if (strcmp(name, "calendar") == 0) {
        menu->page = GC_PAGE_CALENDAR;
        menu->face = GC_FACE_CALENDAR;
    } else if (strcmp(name, "options") == 0) {
        menu->page = GC_PAGE_OPTIONS;
        menu->face = GC_FACE_OPTIONS;
    } else if (strcmp(name, "cards") == 0) {
        menu->page = GC_PAGE_CARDS;
        menu->face = GC_FACE_MEMORY_CARD;
    } else if (strcmp(name, "card-action") == 0 || strcmp(name, "card-confirm") == 0) {
        menu->page =
            !strcmp(name, "card-action") ? GC_PAGE_CARD_ACTION : GC_PAGE_CARD_CONFIRM;
        menu->face = GC_FACE_MEMORY_CARD;
    } else if (strcmp(name, "disc") == 0) {
        menu->page = GC_PAGE_DISC;
        menu->face = GC_FACE_GAME_PLAY;
    } else {
        return false;
    }
    return true;
}

typedef struct {
    const char *ipl;
    const char *output;
    const char *page;
    const char *cards[2];
    const char *disc;
    gc_region region;
    int language;
    double seconds;
} RenderOptions;

static bool parse_options(int argc, char **argv, RenderOptions *options) {
    *options = (RenderOptions){.page = "cube", .region = GC_REGION_USA, .language = -1};
    bool explicit_ipl = false;
    for (int index = 1; index < argc; index++) {
        if (index + 1 >= argc)
            return false;
        if (strcmp(argv[index], "--ipl") == 0) {
            options->ipl = argv[++index];
            explicit_ipl = true;
        } else if (strcmp(argv[index], "--region") == 0) {
            if (!gc_option_region(argv[++index], &options->region))
                return false;
        } else if (strcmp(argv[index], "--language") == 0) {
            static const char *const names[] = {"en", "de", "fr", "es",
                                                "it", "nl", "ja"};
            const char *name = argv[++index];
            options->language = -1;
            for (unsigned choice = 0; choice < 7; choice++)
                if (!strcmp(name, names[choice]))
                    options->language = (int)choice;
            if (options->language < 0)
                return false;
        } else if (strcmp(argv[index], "--output") == 0)
            options->output = argv[++index];
        else if (strcmp(argv[index], "--card-a") == 0)
            options->cards[0] = argv[++index];
        else if (strcmp(argv[index], "--card-b") == 0)
            options->cards[1] = argv[++index];
        else if (strcmp(argv[index], "--disc") == 0)
            options->disc = argv[++index];
        else if (strcmp(argv[index], "--page") == 0)
            options->page = argv[++index];
        else if (strcmp(argv[index], "--time") == 0) {
            if (!gc_option_seconds(argv[++index], &options->seconds))
                return false;
        } else
            return false;
    }
    if (!options->output)
        return false;
    if (options->language >= 0 && (options->region == GC_REGION_EUROPE
                                       ? options->language == GC_LANGUAGE_JAPANESE
                                       : options->language != GC_LANGUAGE_ENGLISH &&
                                             options->language != GC_LANGUAGE_JAPANESE))
        return false;
    if (!explicit_ipl) {
        options->ipl = gc_option_default_ipl(options->region);
    }
    return true;
}

/* Media stays owned by main so every partial load has one release path. */
static bool load_media(const RenderOptions *options, gc_menu *menu,
                       gc_card_image cards[2], GcDisc *disc) {
    for (unsigned slot = 0; slot < 2; ++slot) {
        if (!options->cards[slot])
            continue;
        gc_card_image_open_present(&cards[slot], options->cards[slot],
                                   options->region == GC_REGION_JAPAN ? 1 : 0);
        if (!cards[slot].bytes)
            return false;
        gc_menu_set_card(menu, slot, &cards[slot].card);
    }
    if (options->disc) {
        if (gc_disc_load(options->disc, disc) != GC_DISC_IMAGE_OK)
            return false;
        const GcDiscMetadata *metadata =
            gc_disc_metadata(disc, menu->settings.language);
        gc_menu_set_disc(menu, GC_DISC_READY,
                         metadata ? metadata->game_name : disc->header_title,
                         metadata ? metadata->company : "");
    }
    return true;
}

static bool capture_frame(const RenderOptions *options, gc_menu *menu,
                          const gc_card_image cards[2], const GcDisc *disc) {
    CcPlatform *platform = cc_platform_create("Frame capture", 640, 480);
    GcScene scene = {0};
    FILE *output = NULL;
    bool okay = platform && gc_scene_init(&scene, platform, options->ipl);
    if (okay)
        okay = gc_scene_set_cards(&scene, cards) && gc_scene_set_disc(&scene, disc);
    if (okay) {
        output = gc_tool_output_open(options->output);
        okay = output != NULL;
    }
    if (okay) {
        gc_scene_draw(&scene, menu);
        okay = gc_software_write_stream(platform, output);
    }
    if (output && fclose(output))
        okay = false;
    gc_scene_destroy(&scene);
    cc_platform_destroy(platform);
    return okay;
}

static void usage(void) {
    fprintf(stderr, "Usage: gamecube-render --output Files/frame.ppm\n"
                    "       [--ipl Files/path/IPL.bin] [--time seconds]\n"
                    "       [--region USA|EUR|JAP] [--language en|de|fr|es|it|nl|ja]\n"
                    "       [--page startup|cube|calendar-face|options-face|cards-face|"
                    "disc-face|calendar|options|cards|card-action|card-confirm|disc]\n"
                    "       [--card-a Files/input.raw] [--card-b Files/input.raw]\n"
                    "       [--disc Files/game.iso]\n");
}

int main(int argc, char **argv) {
    RenderOptions options;
    if (!parse_options(argc, argv, &options)) {
        usage();
        return EXIT_FAILURE;
    }
    gc_menu *menu = calloc(1, sizeof(*menu));
    if (!menu)
        return EXIT_FAILURE;
    gc_menu_init(menu, options.region);
    if (options.language >= 0)
        menu->settings.language = (gc_language)options.language;
    if (!set_page(menu, options.page)) {
        free(menu);
        usage();
        return EXIT_FAILURE;
    }
    menu->startup_elapsed = options.seconds;
    menu->page_elapsed = options.seconds;
    gc_card_image cards[2] = {{0}, {0}};
    GcDisc disc = {0};
    bool okay = load_media(&options, menu, cards, &disc) &&
                capture_frame(&options, menu, cards, &disc);
    for (unsigned slot = 0; slot < 2; ++slot)
        gc_card_image_free(&cards[slot]);
    gc_disc_destroy(&disc);
    free(menu);
    if (!okay)
        fprintf(stderr, "Could not decode the IPL or write the frame.\n");
    return okay ? EXIT_SUCCESS : EXIT_FAILURE;
}
