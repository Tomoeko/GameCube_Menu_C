#include "gamecube/texture_collection.h"
#include "gamecube/ipl_model.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void put_u16(uint8_t *bytes, unsigned value) {
    bytes[0] = (uint8_t)(value >> 8);
    bytes[1] = (uint8_t)value;
}

static void put_u32(uint8_t *bytes, uint32_t value) {
    bytes[0] = (uint8_t)(value >> 24);
    bytes[1] = (uint8_t)(value >> 16);
    bytes[2] = (uint8_t)(value >> 8);
    bytes[3] = (uint8_t)value;
}

static void build_collection(uint8_t bytes[160]) {
    memset(bytes, 0, 160);
    memcpy(bytes, "TXH0", 4);
    put_u16(bytes + 4, 2);
    for (unsigned index = 0; index < 2; ++index) {
        uint8_t *row = bytes + 32 + 32 * index;
        put_u16(row + 2, 8);
        put_u16(row + 4, 8);
        row[6] = (uint8_t)index;
        row[7] = (uint8_t)index;
        row[20] = row[21] = 1;
        put_u32(row + 28, 64);
    }
    memset(bytes + 96, 0xff, 32);
    memset(bytes + 128, 0x12, 32);
}

static void test_pool_and_bounds(void) {
    uint8_t bytes[160];
    GcTextureCollection collection = {0};
    const GcIplImage *image;

    build_collection(bytes);
    assert(gc_texture_collection_decode(bytes, sizeof(bytes), &collection));
    assert(collection.count == 2);
    image = gc_texture_collection_image(&collection, 0);
    assert(image && image->width == 8 && image->height == 8);
    assert(image->rgba[0] == 255 && image->rgba[3] == 255);
    image = gc_texture_collection_image(&collection, 1);
    assert(image && image->rgba[0] == 17 && image->rgba[4] == 34);
    assert(collection.information[1].wrap_s == 1 &&
           collection.information[1].wrap_t == 1);
    assert(!gc_texture_collection_image(&collection, 2));
    assert(!gc_texture_collection_decode(bytes, sizeof(bytes), &collection));
    gc_texture_collection_destroy(&collection);
    assert(collection.count == 0 && !collection.images && !collection.information);
    assert(!gc_texture_collection_decode(bytes, 159, &collection));
    assert(!collection.images);
    put_u32(bytes + 60, 32); /* Pixels inside BTI table. */
    assert(!gc_texture_collection_decode(bytes, sizeof(bytes), &collection));
    build_collection(bytes);
    put_u32(bytes + 92, UINT32_MAX);
    assert(!gc_texture_collection_decode(bytes, sizeof(bytes), &collection));
    build_collection(bytes);
    bytes[70] = 3; /* Unknown wrap mode. */
    assert(!gc_texture_collection_decode(bytes, sizeof(bytes), &collection));
    build_collection(bytes);
    put_u16(bytes + 4, 513);
    assert(!gc_texture_collection_decode(bytes, sizeof(bytes), &collection));
}

static void test_resource_table_and_compression(void) {
    uint8_t bytes[62] = {0};
    GcIplResourceTable table = {0};
    GcIplResourceTable saved;
    GcIplResource resource = {0};
    const uint8_t *view;
    size_t size;

    put_u32(bytes + 16, 2);
    put_u32(bytes + 20, 16);
    put_u32(bytes + 24, 24);
    memcpy(bytes + 32, "raw data", 8);
    memcpy(bytes + 40, "Yay0", 4);
    put_u32(bytes + 44, 2);
    put_u32(bytes + 48, 20);
    put_u32(bytes + 52, 20);
    put_u32(bytes + 56, 0xc0000000);
    bytes[60] = 0x12;
    bytes[61] = 0x34;
    assert(gc_ipl_resource_table_decode(bytes, sizeof(bytes), 16, &table));
    assert(table.count == 2);
    assert(gc_ipl_resource_bytes(&table, 0, &view, &size));
    assert(view == bytes + 32 && size == 8);
    assert(gc_ipl_resource_unpack(&table, 0, &resource));
    assert(resource.byte_count == 8 && memcmp(resource.bytes, "raw data", 8) == 0);
    assert(!gc_ipl_resource_unpack(&table, 0, &resource));
    gc_ipl_resource_destroy(&resource);
    assert(gc_ipl_resource_unpack(&table, 1, &resource));
    assert(resource.byte_count == 2 && resource.bytes[0] == 0x12 &&
           resource.bytes[1] == 0x34);
    gc_ipl_resource_destroy(&resource);
    assert(!gc_ipl_resource_bytes(&table, 2, &view, &size));
    saved = table;
    put_u32(bytes + 24, 16);
    assert(!gc_ipl_resource_table_decode(bytes, sizeof(bytes), 16, &table));
    assert(memcmp(&table, &saved, sizeof(table)) == 0);
    put_u32(bytes + 24, 24);
    assert(gc_ipl_resource_table_decode(bytes, sizeof(bytes), 16, &table));
    put_u32(bytes + 44, UINT32_MAX);
    assert(!gc_ipl_resource_unpack(&table, 1, &resource));
    assert(!resource.bytes);
    put_u32(bytes + 44, 3);
    assert(!gc_ipl_resource_unpack(&table, 1, &resource));
    assert(!resource.bytes);
    put_u32(bytes + 20, 4);
    assert(!gc_ipl_resource_table_decode(bytes, sizeof(bytes), 16, &table));
}

static void test_native_grid(void) {
    GcLayoutVertex vertices[4];
    GcLayoutVertex saved[4];

    assert(gc_menu_grid_quad(0, vertices));
    assert(vertices[0].x == 0 && vertices[0].y == 0);
    assert(vertices[2].x == 32 && vertices[2].y == 32);
    assert(vertices[2].u == 2 && vertices[2].v == 2);
    assert(gc_menu_grid_quad(14, vertices));
    assert(vertices[0].x == 32 && vertices[0].y == 0);
    assert(gc_menu_grid_quad(265, vertices));
    assert(vertices[0].x == 576 && vertices[0].y == 416);
    assert(vertices[2].x == 608 && vertices[2].y == 448);
    memcpy(saved, vertices, sizeof(saved));
    assert(!gc_menu_grid_quad(266, vertices));
    assert(memcmp(saved, vertices, sizeof(saved)) == 0);
}

static void test_original_rom(const char *path) {
    GcText text = {0};
    GcLayouts layouts;
    GcMenuTextures textures = {0};
    gc_menu menu;
    GcLayoutPane pane;
    GcLayoutVertex vertices[4];
    GcLayoutFrameQuad frame_quad;

    assert(gc_text_load(path, &text));
    assert(gc_layouts_index(&text, &layouts));
    assert(gc_menu_textures_decode(&text, &textures));
    assert(textures.collection.count == (text.europe ? 97 : 42));
    assert(textures.grid.width == 8 && textures.grid.height == 8);
    assert(textures.card_numbers.width == 110 && textures.card_numbers.height == 14);
    for (unsigned digit = 0; digit < 10; ++digit)
        assert(textures.digits[digit].width == 16 &&
               textures.digits[digit].height == 22);
    for (unsigned language = 0; language < 7; ++language) {
        bool available = text.europe ? language != GC_LANGUAGE_JAPANESE
                                     : language == GC_LANGUAGE_ENGLISH ||
                                           language == GC_LANGUAGE_JAPANESE;
        if (!available) {
            assert(!textures.weekdays[language][0].rgba);
            continue;
        }
        gc_menu_init(&menu, text.europe ? GC_REGION_EUROPE : GC_REGION_USA);
        menu.settings.language = (gc_language)language;
        menu.clock = (gc_date_time){2024, 2, 29, 12, 34, 56};
        for (unsigned day = 0; day < 7; ++day) {
            const GcIplImage *image = &textures.weekdays[language][day];
            assert(image->width == (language == GC_LANGUAGE_JAPANESE ? 24 : 48));
            assert(image->height == 24 && image->rgba);
        }
        for (unsigned mode = 0; mode < 2; ++mode)
            assert(textures.sound[language][mode].width == 120 &&
                   textures.sound[language][mode].height == 24);
        for (unsigned group = 0; group < GC_LAYOUT_GROUP_COUNT; ++group) {
            const GcLayoutTable *table =
                gc_layout_table(&layouts, (gc_language)language, (GcLayoutGroup)group);
            for (unsigned index = 0; index < table->pane_count; ++index) {
                assert(gc_layout_pane(table, index, &pane));
                assert(pane.texture < textures.collection.count);
                const GcIplImage *image = gc_menu_textures_pane(
                    &textures, &menu, (GcLayoutGroup)group, &pane);
                if (group == GC_LAYOUT_OPTIONS_FACE &&
                    (!memcmp(pane.name, "arw", 3) || !memcmp(pane.name, "zrw", 3)))
                    assert(!image);
                else
                    assert(image && image->rgba);
            }
            for (unsigned index = 0; index < table->frame_count; ++index) {
                GcLayoutFrame frame;
                assert(gc_layout_frame(table, index, &frame));
                for (unsigned part = 0; part < 9; ++part)
                    assert(gc_layout_frame_quad(&frame, textures.collection.images,
                                                textures.collection.count, part,
                                                0xffffffff, &frame_quad));
            }
        }
        const GcLayoutTable *calendar =
            gc_layout_table(&layouts, (gc_language)language, GC_LAYOUT_CALENDAR_FACE);
        assert(gc_layout_find_pane(calendar, "cal1", 0, &pane));
        assert(gc_menu_textures_pane(&textures, &menu, GC_LAYOUT_CALENDAR_FACE,
                                     &pane) == &textures.digits[2]);
        assert(gc_layout_find_pane(calendar, "tim6", 0, &pane));
        assert(gc_menu_textures_pane(&textures, &menu, GC_LAYOUT_CALENDAR_FACE,
                                     &pane) == &textures.digits[6]);
        assert(gc_layout_find_pane(calendar, "week", 0, &pane));
        assert(
            gc_menu_textures_pane(&textures, &menu, GC_LAYOUT_CALENDAR_FACE, &pane) ==
            &textures.weekdays[language][4]); /* Leap-day Thursday. */
        menu.clock = (gc_date_time){2026, 10, 1, 12, 34, 56};
        assert(gc_menu_textures_pane(&textures, &menu, GC_LAYOUT_CALENDAR_FACE,
                                     &pane) == &textures.weekdays[language][4]);
        menu.clock.day = 2;
        assert(gc_menu_textures_pane(&textures, &menu, GC_LAYOUT_CALENDAR_FACE,
                                     &pane) == &textures.weekdays[language][5]);
        /* The native PAL initializer's language pools are not sequential.
         * Verify English/German Thursday and Friday against the original
         * resource indices at 0x8130b518; NTSC 0x8130b3ec uses English31. */
        if (language == GC_LANGUAGE_ENGLISH || language == GC_LANGUAGE_GERMAN) {
            unsigned base =
                text.europe ? (language == GC_LANGUAGE_ENGLISH ? 71u : 171u) : 31u;
            GcIplResourceTable resources;
            assert(gc_ipl_resource_table_decode(
                text.rom, text.rom_size, text.europe ? 0x82040 : 0x5f240, &resources));
            for (unsigned weekday = 4; weekday <= 5; ++weekday) {
                GcIplResource resource = {0};
                GcIplImage original = {0};
                assert(gc_ipl_resource_unpack(&resources, base + weekday, &resource));
                assert(gc_ipl_texture_decode(resource.bytes, resource.byte_count,
                                             &original));
                const GcIplImage *decoded = &textures.weekdays[language][weekday];
                assert(original.width == decoded->width &&
                       original.height == decoded->height);
                assert(!memcmp(original.rgba, decoded->rgba,
                               (size_t)original.width * original.height * 4));
                gc_ipl_image_destroy(&original);
                gc_ipl_resource_destroy(&resource);
            }
        }
        const GcLayoutTable *options =
            gc_layout_table(&layouts, (gc_language)language, GC_LAYOUT_OPTIONS_FACE);
        assert(gc_layout_find_pane(options, "num1", 0, &pane));
        menu.settings.screen_position = -32;
        assert(gc_menu_textures_pane(&textures, &menu, GC_LAYOUT_OPTIONS_FACE, &pane) ==
               &textures.digits[3]);
        assert(gc_layout_find_pane(options, "arwl", 0, &pane));
        assert(gc_menu_textures_pane(&textures, &menu, GC_LAYOUT_OPTIONS_FACE, &pane));
        menu.settings.screen_position = 32;
        assert(!gc_menu_textures_pane(&textures, &menu, GC_LAYOUT_OPTIONS_FACE, &pane));
        assert(gc_layout_find_pane(options, "sond", 0, &pane));
        assert(gc_menu_textures_pane(&textures, &menu, GC_LAYOUT_OPTIONS_FACE, &pane) ==
               &textures.sound[language][GC_SOUND_STEREO]);
    }
    assert(gc_menu_card_number_quad(&textures, 101, 231, 46, 0, vertices));
    assert(vertices[0].x == 214 && vertices[0].y == 39);
    assert(vertices[2].x == 224 && vertices[2].y == 53);
    assert(fabsf(vertices[0].u - 1.0f / 11) < 0.00001f);
    assert(gc_menu_card_number_quad(&textures, 101, 231, 46, 2, vertices));
    assert(vertices[0].x == 238 && vertices[2].x == 248);
    assert(!gc_menu_card_number_quad(&textures, 101, 231, 46, 3, vertices));
    assert(!gc_menu_card_number_quad(&textures, 10000, 231, 46, 0, vertices));
    assert(!gc_menu_card_number_quad(&textures, 1, NAN, 46, 0, vertices));
    gc_menu_textures_destroy(&textures);
    assert(!textures.grid.rgba && !textures.collection.images);
    gc_text_destroy(&text);
}

int main(int argc, char **argv) {
    test_pool_and_bounds();
    test_resource_table_and_compression();
    test_native_grid();
    for (int index = 1; index < argc; ++index)
        test_original_rom(argv[index]);
    puts("Native TXH0 textures, resource indices and menu glyph tests passed.");
    return 0;
}
