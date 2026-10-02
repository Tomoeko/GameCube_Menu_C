#ifndef GAMECUBE_CARD_OPERATION_H
#define GAMECUBE_CARD_OPERATION_H

#include "gamecube/menu.h"

#define GC_CARD_OPERATION_BUSY INT32_C(-21)
#define GC_CARD_OPERATION_NO_DESTINATION INT32_C(-23)
#define GC_CARD_OPERATION_PAD_A UINT16_C(0x0100)
#define GC_CARD_OPERATION_PAD_B UINT16_C(0x0200)
#define GC_CARD_OPERATION_PIECES 12
#define GC_CARD_OPERATION_LAYOUT_TICKS 40
#define GC_CARD_OPERATION_SHRINK_TICKS 20
#define GC_CARD_OPERATION_PIECE_TICKS 10

typedef enum {
    GC_CARD_OPERATION_IDLE,
    GC_CARD_OPERATION_IO,
    GC_CARD_OPERATION_METADATA,
    GC_CARD_OPERATION_GEOMETRY,
    GC_CARD_OPERATION_ACKNOWLEDGE
} GcCardOperationPhase;

/* Values correspond to the original controller's operation-state field. */
typedef enum {
    GC_CARD_OPERATION_NATIVE_IDLE = 0,
    GC_CARD_OPERATION_NATIVE_FORMAT = 1,
    GC_CARD_OPERATION_NATIVE_COPY = 2,
    GC_CARD_OPERATION_NATIVE_MOVE = 3,
    GC_CARD_OPERATION_NATIVE_ERASE = 4,
    GC_CARD_OPERATION_NATIVE_COPIED = 5,
    GC_CARD_OPERATION_NATIVE_MOVED = 6,
    GC_CARD_OPERATION_NATIVE_ERASED = 7,
    GC_CARD_OPERATION_NATIVE_FORMATTED = 8
} GcCardOperationNativeState;

typedef struct {
    int32_t result; /* Native BUSY, zero success, or the adapter's error code. */
    bool worker_busy;
    bool metadata_changed;
    bool card_ready;
    uint8_t destination_file_index;
} GcCardOperationPoll;

typedef struct {
    GcCardOperationPoll slots[2];
    /* Native card-face draw/update flag. An inactive face freezes geometry. */
    bool geometry_active;
    /* All three card page faders exceed172, and any external cell motion has
     * settled. The controller additionally gates its own erase and relayout.
     */
    bool geometry_ready;
    uint16_t pressed_buttons; /* Already filtered native A/B edge events. */
} GcCardOperationInput;

typedef struct {
    GcCardOperationNativeState native_state;
    gc_card_action action;
    unsigned source_slot;
    unsigned source_file_index;
    unsigned destination_file_index;
    int32_t result;
    unsigned progress_poll_count;
    unsigned layout_tick;
    unsigned shrink_remaining;
    float shrink_fraction;
    uint8_t piece_ticks[GC_CARD_OPERATION_PIECES];
    uint8_t piece_delays[GC_CARD_OPERATION_PIECES];
    bool selected_special;
    bool erase_active;
    bool pieces_active;
    bool layout_active;
    bool input_ready;
} GcCardOperation;

typedef struct {
    unsigned sound_events[8];
    unsigned sound_event_count;
    bool io_requested;
    bool geometry_begin;
    bool geometry_aborted;
    bool metadata_completed;
    bool layout_begin;
    bool pieces_begin;
    bool pieces_finished;
    bool presentation_ready;
    bool dismissed;
    uint8_t metadata_slots;
} GcCardOperationEvents;

/* Start after the original confirmation's Yes decision. The caller performs
 * its local storage request when io_requested is emitted. Copy/move require
 * a ready destination; other actions ignore destination_ready. Explicit
 * piece delays are the original random values0..3. NULL selects simultaneous
 * pieces for deterministic inspection without inventing a random seed.
 */
bool gc_card_operation_begin(GcCardOperation *operation, gc_card_action action,
                             unsigned source_slot, unsigned source_file_index,
                             bool destination_ready,
                             const uint8_t piece_delays[GC_CARD_OPERATION_PIECES],
                             GcCardOperationEvents *events);

/* One native video update. Each slot's metadata notification is processed
 * before the operation poll. There are TWO polls per video tick, as in the
 * original card dispatcher, followed by geometry and acknowledgment input.
 * A successful adapter result does not fabricate a metadata notification.
 * Hold worker_busy until the actual service has finished publishing metadata.
 */
bool gc_card_operation_step(GcCardOperation *operation,
                            const GcCardOperationInput *input,
                            GcCardOperationEvents *events);
GcCardOperationPhase gc_card_operation_phase(const GcCardOperation *operation);
/* Apply filtered A/B edges using the readiness from the latest video tick.
 * This does not add an extra geometry or service update between video ticks.
 */
bool gc_card_operation_press(GcCardOperation *operation, uint16_t pressed_buttons,
                             GcCardOperationEvents *events);

/* The adapter reports actual service state; no host storage implementation
 * or timing is imposed here. This convenience function obtains the two slot
 * snapshots and delegates to the same recovered controller.
 */
typedef bool (*GcCardOperationPollAdapter)(void *context, unsigned slot,
                                           GcCardOperationPoll *poll);
bool gc_card_operation_step_adapter(GcCardOperation *operation,
                                    GcCardOperationPollAdapter adapter, void *context,
                                    bool geometry_active, bool geometry_ready,
                                    uint16_t pressed_buttons,
                                    GcCardOperationEvents *events);

#endif
