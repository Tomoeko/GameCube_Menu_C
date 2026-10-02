#ifndef GAMECUBE_NAVIGATION_SOUND_H
#define GAMECUBE_NAVIGATION_SOUND_H

#include "gamecube/menu.h"

/* Native cube navigation cues follow the resulting camera state change.
 * A/B and cardinal directions therefore use the same transition sound.
 * Zero means the transition is outside the cube navigation controller.
 */
unsigned gc_navigation_sound(gc_page previous, gc_page current);

#endif
