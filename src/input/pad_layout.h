/*
 * The controller driver's record layouts as this SDK understands them, and the mapping from
 * one raw record to the SDK's pad state. Private to the input subsystem and its host tests:
 * nothing here is API, and the tests include it by its source path on purpose.
 */
#ifndef OOPS_INPUT_PAD_LAYOUT_H
#define OOPS_INPUT_PAD_LAYOUT_H

#include <stdint.h>
#include <stddef.h>
#include "oops/input.h"

/* Hardware ScePadTouch and ScePadData layout */
typedef struct {
    uint16_t x;
    uint16_t y;
    uint8_t  finger;
    uint8_t  pad[3];
} ScePadTouch;

typedef struct {
    uint8_t     fingers;
    uint8_t     pad1[3];
    uint32_t    pad2;
    ScePadTouch touch[2];
} ScePadTouchData;

typedef struct {
    uint32_t buttons;                              /* offset  0 */
    struct { uint8_t x; uint8_t y; } leftStick;    /* offset  4 */
    struct { uint8_t x; uint8_t y; } rightStick;   /* offset  6 */
    struct { uint8_t l2; uint8_t r2; } analogButtons; /* offset  8 */
    uint16_t    padding;                           /* offset 10 */
    struct { float x, y, z, w; } quat;            /* offset 12 (orientation) */
    struct { float x, y, z; }    accel;           /* offset 28 (acceleration) */
    struct { float x, y, z; }    vel;             /* offset 40 (angular velocity) */
    ScePadTouchData touchData;                     /* offset 52 */
    uint8_t     connected;                         /* offset 76 */
    uint8_t     _align[3];                         /* offset 77 */
    uint64_t    timestamp;                         /* offset 80 */
    uint8_t     reserved[64];                      /* oversize for safety */
} ScePadDataInternal;

/* The offsets the comments promise, enforced: a field moved by an edit fails here. */
_Static_assert(offsetof(ScePadDataInternal, leftStick) == 4, "leftStick at 4");
_Static_assert(offsetof(ScePadDataInternal, rightStick) == 6, "rightStick at 6");
_Static_assert(offsetof(ScePadDataInternal, analogButtons) == 8, "analogButtons at 8");
_Static_assert(offsetof(ScePadDataInternal, quat) == 12, "quat at 12");
_Static_assert(offsetof(ScePadDataInternal, accel) == 28, "accel at 28");
_Static_assert(offsetof(ScePadDataInternal, vel) == 40, "vel at 40");
_Static_assert(offsetof(ScePadDataInternal, touchData) == 52, "touchData at 52");
_Static_assert(offsetof(ScePadDataInternal, connected) == 76, "connected at 76");
_Static_assert(offsetof(ScePadDataInternal, timestamp) == 80, "timestamp at 80");

/*
 * scePadRead() writes `num` consecutive driver records, so the batched read stride is the
 * record size the driver uses. ScePadDataInternal carries an oversized tail, which is safe
 * for the single-record read and wrong for a batch: every record after the first would be
 * parsed at the wrong offset. That size is not yet captured, so 0 keeps the batched read
 * refusing. When obSCEne confirms it, set it here; the assert then forces the layout to
 * match before the batch is trusted.
 */
#define OOPS_PAD_RECORD_BYTES 0
#if OOPS_PAD_RECORD_BYTES
_Static_assert(sizeof(ScePadDataInternal) == OOPS_PAD_RECORD_BYTES,
               "ScePadDataInternal must be exactly one driver record for the batched read");
#endif

/* Map one raw platform record into the SDK's pad-state shape. Shared by the single-state
 * poll and the batched read, so the mapping lives in exactly one place. Does not clear
 * out_state - every field it reports is written here, and callers zero first. */
void oops_input_map_record(oops_pad_state_t *out_state, const ScePadDataInternal *raw);

#endif /* OOPS_INPUT_PAD_LAYOUT_H */
