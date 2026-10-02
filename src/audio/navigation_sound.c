#include "gamecube/navigation_sound.h"

static bool full_page(gc_page page) {
    return page == GC_PAGE_CALENDAR || page == GC_PAGE_OPTIONS ||
           page == GC_PAGE_CARDS || page == GC_PAGE_DISC;
}

unsigned gc_navigation_sound(gc_page previous, gc_page current) {
    /* USA/JAP10d80: HOME to face3, face to HOME4, face to page5.
     * Native page controllers return to the face with6 (26620,26d8c,
     * 276c4 and the card dispatcher). These IDs do not depend on a key.
     */
    if (previous == GC_PAGE_CUBE && current == GC_PAGE_FACE)
        return 3;
    if (previous == GC_PAGE_FACE && current == GC_PAGE_CUBE)
        return 4;
    if (previous == GC_PAGE_FACE && full_page(current))
        return 5;
    if (full_page(previous) && current == GC_PAGE_FACE)
        return 6;
    return 0;
}
