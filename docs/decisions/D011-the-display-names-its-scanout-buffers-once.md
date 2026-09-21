# D011 - The display names its scanout buffers once, and they must be plain

**Status:** decided
**Date:** 2026-09-21

## The choice

`agc_display_open_adopting` takes the buffers a renderer wants scanned out, names them to VideoOut
in the one registration the platform allows, and reports their flip indices. There is no call to
add a buffer afterwards, and there will not be one, because the platform does not have it.

A renderer that wants its own target shown without a copy hands it over **at open**, and the target
must carry **no compression metadata**.

## Why registration happens exactly once

Measured, obSCEne `REQ-20260921T1202Z-9a4c`:

| attempt on a handle that already has buffers | result |
|---|---|
| re-register the same set at index 0 | `0x80290010` |
| register a larger set at index 0 | `0x80290010` |
| register a set at index 1 | `0x80290010` |
| register one buffer at index 2 | `0x80290001` |
| `sceVideoOutUnregisterBuffer` / `sceVideoOutUnregisterBuffers` | **not exported by `libSceVideoOut`** |
| open a second handle on the same output | refused, `0x80290001` |

`0x80290010` is `SCE_VIDEO_OUT_ERROR_SLOT_OCCUPIED`. So a buffer set cannot be extended, replaced,
or released. Whatever is named at open is what this display can ever show.

That is why the entry point is an `open` variant rather than an `adopt` on a live display. An
earlier version of this API was the latter - `agc_display_adopt_buffer`, added and removed the same
day - and it could not have worked at any point in its life. The shape of the call now matches the
shape of the constraint.

The practical consequence for a caller is worth stating plainly: **the buffer must already exist
before the display opens.** A renderer whose target is allocated lazily, on its first frame, has to
force that allocation first. oops-mesa does exactly this - it builds the drawable's colour image,
then opens the display with it.

## Why the buffer must be uncompressed

This display registers its buffers with `dcc_control = 0` - delta colour compression off. The
display controller therefore reads whatever it is given as raw pixels.

Hand it a compressed surface and it scans the compressed bytes as colour. The result is not a
failure, a refusal or a black screen: it is a **plausible-looking wrong picture**. oops-mesa put
one on the panel - geometry correctly placed, sized and oriented, with a sparse lattice of correct
pixels inside it - and the frame hash was perfect throughout, because the hash was taken from the
render and not from what the display read (oops-mesa worklog 070, obSCEne
`REQ-20260921T1349Z-8b52`).

So an adopted buffer must be a plain surface, and a caller should check that rather than assume it.
The check that works is the **allocation size**: a plain `64KB_R_X` target is the height padded to
a multiple of 128 and nothing more - `1920 * 1152 * 4 = 8847360` at 1080p. Mesa's default
allocation was 49152 bytes larger, and that excess was its displayable DCC metadata.

## What the layout has to be, and the good news about it

`agc_display_scanout_layout` reports what this display scans: `tiling_mode = 0` is the GPU's
`64KB_R_X` render-target swizzle, `tiling_mode = 1` is linear rows.
`sceVideoOutSetBufferAttribute2` writes that argument straight through into offset `0x04` of the
attribute block without masking or validating it (`-8b52`).

**A GPU-rendered `64KB_R_X` target is byte-identical to what this display's own tiler produces.**
That is measured across a whole frame, not inferred from a block: `-8b52` compared a Mesa-drawn
1920x1080 target (`CB_COLOR0_ATTRIB3 = 0x0dc6c000`, `SW_MODE 27`) against the display tiler's
detile over all 135 64 KiB blocks - **2073600 pixels, zero mismatches**.

So a renderer does not need to match some private layout this library invented. It renders into
`64KB_R_X`, which is what the hardware's colour block produces anyway, and the display can show it.

Linear is not an escape route at display size: `REQ-20260920T0745Z-2d7f` measured the RDNA2 colour
block **dropping pixels** on a 1920x1080 target with a linear swizzle.

## What this is worth

oops-mesa's present went from 37229 us to 3675 us by taking it - a tenfold cut, with the CPU no
longer touching a single pixel of the frame. The same saving is available to oops-gl, which asks
for it in `REQ-20260919T1927Z-7e21` against its own linear scratch copy, and the API was written
for both callers rather than for the one that needed it first.

## What would reverse this

A measurement showing `dcc_control` can be configured to match a compressed surface's metadata.
`-8b52` names that as the untaken alternative to disabling compression, and it would be worth
having: DCC exists to save bandwidth, and refusing it is a cost as well as a simplification. It
would change the *precondition* on an adopted buffer, not the single-shot registration around it.
