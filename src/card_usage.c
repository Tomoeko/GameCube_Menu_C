#include "gamecube/card_usage.h"

enum { MINIMUM_ALPHA = 60, MAXIMUM_ALPHA = 255, ALPHA_STEP = 4 };

static uint8_t advance_alpha(uint8_t value, bool ready, uint64_t ticks) {
    if (!ticks)
        return value;
    if (!value) {
        value = MINIMUM_ALPHA;
        --ticks;
    }
    unsigned distance = ready ? MAXIMUM_ALPHA - value : value - MINIMUM_ALPHA;
    unsigned duration = (distance + ALPHA_STEP - 1) / ALPHA_STEP;
    if (ticks >= duration)
        return ready ? MAXIMUM_ALPHA : MINIMUM_ALPHA;
    unsigned change = (unsigned)ticks * ALPHA_STEP;
    return (uint8_t)(ready ? value + change : value - change);
}

bool gc_card_usage_advance(GcCardUsage *state, const gc_card cards[2], uint64_t ticks) {
    if (!state || !cards)
        return false;
    GcCardUsage candidate = *state;
    for (unsigned slot = 0; slot < 2; ++slot) {
        const gc_card *card = &cards[slot];
        if ((state->alpha[slot] && state->alpha[slot] < MINIMUM_ALPHA) ||
            card->status < GC_CARD_ABSENT || card->status > GC_CARD_DAMAGED ||
            card->file_count > GC_CARD_FILE_LIMIT)
            return false;
        bool ready = card->status == GC_CARD_READY;
        if (ready) {
            if (!card->capacity_blocks)
                return false;
            uint32_t used = 0;
            for (size_t index = 0; index < card->file_count; ++index) {
                if (!card->files[index].blocks)
                    return false;
                used += card->files[index].blocks;
            }
            if (used > card->capacity_blocks)
                return false;
            /* USA/JAP 0x81311994 and PAL 0x8131247c subtract the truncated
             * free percentage. A partially used card therefore rounds up. */
            if (ticks)
                candidate.level[slot] =
                    (uint8_t)(MAXIMUM_ALPHA - (card->capacity_blocks - used) *
                                                  MAXIMUM_ALPHA /
                                                  card->capacity_blocks);
        }
        candidate.alpha[slot] = advance_alpha(state->alpha[slot], ready, ticks);
    }
    *state = candidate;
    return true;
}

uint8_t gc_card_usage_alpha(const GcCardUsage *state, unsigned slot,
                            uint8_t face_alpha) {
    if (!state || slot > 1)
        return 0;
    return (uint8_t)((unsigned)face_alpha * state->alpha[slot] / MAXIMUM_ALPHA);
}

uint8_t gc_card_usage_fill(const GcCardUsage *state, unsigned slot) {
    if (!state || slot > 1 || state->alpha[slot] < MINIMUM_ALPHA)
        return 0;
    /* USA/JAP 0x81312984: the dim outline remains at alpha60, but its fill
     * reaches zero when the slot is unavailable. PAL uses the same ratio. */
    return (uint8_t)((unsigned)state->level[slot] *
                     (state->alpha[slot] - MINIMUM_ALPHA) /
                     (MAXIMUM_ALPHA - MINIMUM_ALPHA));
}
