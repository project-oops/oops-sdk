#ifndef OOPS_AGC_DRIVER_H
#define OOPS_AGC_DRIVER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Hardware-confirmed libSceAgc / libSceAgcDriver bindings for the direct GPU
 * submission path.
 *
 * Every symbol and arity here was exercised on retail prospero (firmware 12.40,
 * Oberon RDNA2) by obSCEne sweep 20260910-203426-eboot, section 166-agc:
 * subsystem init, queue creation, PM4 DCB submission, compute dispatch and
 * end-of-pipe fence retirement each returned rc 0x0, and the GPU retired the
 * fence (fence-val 0xbeefcafe, fence-hit 0x1) in coherent Onion memory. See
 * obscene/docs/worklog/169-hardware-compute-dispatch-and-fence-retirement.md
 * and this repo's docs/worklog/001-hardware-accelerated-agc-presentation.md.
 *
 * These are the raw platform entry points, declared weak so a host build or a
 * payload that does not resolve libSceAgc still links; a caller must test the
 * pointer before use, exactly as the VideoOut and Kernel externs in
 * agc_display.c do. The loader binds each by NID (recorded beside it) - the
 * SHA-1-derived hash oops-sdk already pins in test_freestd.
 */

/*
 * The descriptor sceAgcDriverSubmitDcb reads. Sixteen bytes. The field that
 * must be exact is `size`: the command-buffer length in DWORDs, NOT bytes.
 * libSceAgcDriver+0x1100 loads it from +0x08 and shifts it left by 4 into the
 * IB_SIZE field (bits 19:0) of the PM4 PACKET3_INDIRECT_BUFFER; a byte count
 * there makes the command processor run past the buffer into uninitialised
 * memory and fault before the fence retires - the defect that held fence-hit at
 * 0x0 until sweep 20260910-203426 (obSCEne worklog 169). gpu_addr (+0x00) and
 * size (+0x08) are disassembly-confirmed; flags and pad are the trailing four
 * bytes, zero on the traced submit and not read by the driver on that path.
 */
typedef struct oops_agc_dcb_desc {
    uint64_t gpu_addr; /* +0x00  GPU virtual address of the DCB, in mapped Onion
                          memory */
    uint32_t size;     /* +0x08  length in DWORDs (32-bit words), never bytes */
    uint16_t flags;    /* +0x0C  submission flags; 0 for a plain submit */
    uint16_t pad;      /* +0x0E  reserved, zero */
} oops_agc_dcb_desc;

_Static_assert(sizeof(oops_agc_dcb_desc) == 16, "AGC DCB descriptor is 16 bytes");
_Static_assert(offsetof(oops_agc_dcb_desc, gpu_addr) == 0x00, "gpu_addr at +0x00");
_Static_assert(offsetof(oops_agc_dcb_desc, size) == 0x08, "size (DWORDs) at +0x08");

/* libSceAgc - subsystem bring-up and shader creation.
 * sceAgcInit(state, version): version 0xd confirmed; writes a 16-byte state
 * block. NID $23LRUSvYu1M. */
__attribute__((weak)) int sceAgcInit(void *state, uint32_t version);
/* sceAgcGetIsTrinityMode(is_trinity): writes 0 or 1; 0 on the base prospero
 * test unit. NID $BfBDZGbti7A. */
__attribute__((weak)) int sceAgcGetIsTrinityMode(uint32_t *is_trinity);
/* sceAgcCreateShader(shader_obj, header, gpu_payload, flags): builds a shader
 * object from a 304-byte RDNA2 container header and a 256-byte-aligned bytecode
 * payload; flags 0. NID $f3dg2CSgRKY. */
__attribute__((weak)) int sceAgcCreateShader(void *shader_obj, const void *header,
                                             void *gpu_payload, uint32_t flags);

/* libSceAgcDriver - queue lifecycle and DCB submission.
 * sceAgcDriverCreateQueue(type, queue_out, flags): type 3 = Direct Command
 * Buffer queue; flags 0. NID $zP4ZNlXLBVg. */
__attribute__((weak)) int sceAgcDriverCreateQueue(uint32_t type, void *queue_out,
                                                  uint32_t flags);
/* sceAgcDriverDestroyQueue(queue). NID $XNbrdwCsZ9A. */
__attribute__((weak)) int sceAgcDriverDestroyQueue(void *queue);
/* sceAgcDriverSubmitDcb(desc): submit one DCB; the GPU executes it and retires
 * its EOP fence. NID $UglJIZjGssM. */
__attribute__((weak)) int sceAgcDriverSubmitDcb(const oops_agc_dcb_desc *desc);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_AGC_DRIVER_H */
