/*
 * The controller driver's record layouts as this SDK understands them, and the
 * mapping from one raw record to the SDK's pad state. Private to the input
 * subsystem and its host tests: nothing here is API, and the tests include it
 * by its source path on purpose.
 */
#ifndef OOPS_INPUT_PAD_LAYOUT_H
#define OOPS_INPUT_PAD_LAYOUT_H

#include "oops/input.h"
#include <stddef.h>
#include <stdint.h>

/* Hardware ScePadTouch and ScePadData layout */
typedef struct {
    uint16_t x;
    uint16_t y;
    uint8_t finger;
    uint8_t pad[3];
} ScePadTouch;

typedef struct {
    uint8_t fingers;
    uint8_t pad1[3];
    uint32_t pad2;
    ScePadTouch touch[2];
} ScePadTouchData;

typedef struct {
    uint32_t buttons; /* offset  0 */
    struct {
        uint8_t x;
        uint8_t y;
    } leftStick; /* offset  4 */
    struct {
        uint8_t x;
        uint8_t y;
    } rightStick; /* offset  6 */
    struct {
        uint8_t l2;
        uint8_t r2;
    } analogButtons;  /* offset  8 */
    uint16_t padding; /* offset 10 */
    struct {
        float x, y, z, w;
    } quat; /* offset 12 (orientation) */
    struct {
        float x, y, z;
    } accel; /* offset 28 (acceleration) */
    struct {
        float x, y, z;
    } vel;                     /* offset 40 (angular velocity) */
    ScePadTouchData touchData; /* offset 52 */
    uint8_t connected;         /* offset 76 */
    uint8_t _align[3];         /* offset 77 */
    uint64_t timestamp;        /* offset 80 */
    uint8_t reserved[32];      /* offset 88: the tail to 120, not read */
} ScePadDataInternal;

/* The offsets the comments promise, enforced: a field moved by an edit fails
 * here. */
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
 * The driver record is 120 bytes: obSCEne handed scePadReadState and scePadRead
 * a 4 KB 0xC7 fill on 12.40 and each rewrote exactly 120 (100-input/read-extent
 * and batched-read, sweep 20260909-110725, app context, nothing attached). The
 * batched read returned one record. In that record the sticks read 128,
 * orientation w and acceleration y read 1.0, and the connected byte at 76 read
 * 0 - written, not left as fill. scePadRead() writes `num` consecutive records,
 * so this size is the batch stride, and the asserts hold the struct to it.
 */
#define OOPS_PAD_RECORD_BYTES 120
_Static_assert(sizeof(ScePadDataInternal) == OOPS_PAD_RECORD_BYTES,
               "ScePadDataInternal must be exactly one driver record for the "
               "batched read");
_Static_assert(offsetof(ScePadDataInternal, reserved) == 88, "tail at 88");

/* Map one raw platform record into the SDK's pad-state shape. Shared by the
 * single-state poll and the batched read, so the mapping lives in exactly one
 * place. Does not clear out_state - every field it reports is written here, and
 * callers zero first. */
void oops_input_map_record(oops_pad_state_t *out_state, const ScePadDataInternal *raw);

#endif /* OOPS_INPUT_PAD_LAYOUT_H */
