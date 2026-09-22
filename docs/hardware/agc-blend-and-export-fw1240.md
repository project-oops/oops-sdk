# Blend constants, dual targets, 3D mip offsets and a fifth export - firmware 12.40

Four questions oops-gl could not answer from its own side, measured by obSCEne on a retail
console and resolved together in **sweep 20260921-run17** on 2026-09-21. Each entry gives the
registers as programmed, the pixel the hardware produced, and what oops-gl has to do about it.

These were the four standing failures in `gl1-probe` at 82/85. Three of them explain a refusal
that was already correct; one of them is a defect with a workaround.

---

## 1. A constant-colour blend reads green from `CB_BLEND_ALPHA`

**Request `-2e9f`** (a re-file: the first attempt, `-4b8d`, put every arm through `BLEND_BYPASS`
so nothing blended and green was never measured at all).

Programmed: `CB_COLOR0_INFO = 0x000088a8`, `CB_BLEND0_CONTROL = 0x600d000d`
(`COLOR_SRCBLEND = BLEND_CONSTANT_COLOR`, dest `ZERO`), fragment export white `(1,1,1,1)`, and
the constant registers set to **red 0.25** (`0x3e800000`), **green 0.50** (`0x3f000000`),
**blue 0.75** (`0x3f400000`), **alpha 1.00** (`0x3f800000`).

Every one of four arms produced the same pixel: **`0xff40ffbf`** - red `0x40`, green `0xff`,
blue `0xbf`, alpha `0xff`.

Red is `0x40`, which is 0.25. Blue is `0xbf`, which is 0.75. Both took their own registers.
**Green came back `0xff`, which is 1.00 - the value in `CB_BLEND_ALPHA` at `0x108`, not the 0.50
in `CB_BLEND_GREEN` at `0x106`.**

The four arms were chosen to kill the obvious explanations, and they did:

| Arm | What it varied | Result |
|---|---|---|
| `arm1-one-packet` | one 4-dword `SET_CONTEXT_REG` at `0x105` | `0xff40ffbf` |
| `arm2-four-packets` | four separate single-register writes, `0x105`-`0x108` | `0xff40ffbf` |
| `arm3-green-last` | arm1, then `0x106` rewritten to 0.0625f | `0xff40ffbf` |
| `arm4-rbplus` | arm1 plus the RB+ registers `0x1d4`-`0x1d8` | `0xff40ffbf` |

So it is not packet shape, not write ordering, and not RB+ downconvert. **The green channel of a
`BLEND_CONSTANT_COLOR` blend unconditionally reads `0x108`.**

**What oops-gl must do.** `glBlendColor(r, g, b, a)` currently writes `0x105`-`0x108` in order.
For any blend factor in the `GL_CONSTANT_COLOR` family, green has to be written to `0x108` as
well or it will read as the alpha constant.

That has a limit worth stating rather than discovering: a blend equation using
`GL_CONSTANT_COLOR` *and* `GL_CONSTANT_ALPHA` at once cannot have both correct, because both
want `0x108`. It is a rare combination and the right answer there is a refusal, not a guess -
`oops-sdk#D009`.

---

## 2. Two colour targets blend correctly, on both

**Request `-5c07`.**

Programmed: `CB_COLOR0_INFO` and `CB_COLOR1_INFO` both `0x88a8`, `CB_TARGET_MASK = 0xff`,
`CB_SHADER_MASK = 0xff`, `SPI_SHADER_COL_FORMAT = 0x99`, and **both** `CB_BLEND0_CONTROL` and
`CB_BLEND1_CONTROL` set to `0x60010001` (`GL_ONE`/`GL_ONE`). Target 0 pre-filled blue, target 1
pre-filled red, pixel shader exporting green to both.

| Arm | Target 0 | Target 1 |
|---|---|---|
| `control-one-target` | `0xff0000ff` | `0x22222222` (untouched) |
| `arm1-mrt` unblended | `0xff0000ff` | `0xff00ff00` |
| `arm2-dual-blend` | `0xff00ff00` | `0xff00ff00` |
| `arm3-dual-blend-rbplus` | `0xff00ff00` | `0xff00ff00` |

Both targets blended, both stable across a repeat read, `fence-hit 0x1`, 512 pixels modified.
**Dual-target blending works.** RB+ changed nothing.

**What this confirms.** `CB_BLEND1_CONTROL` (`0x1e1`) has to be programmed for the second target
- the front-and-back path was failing because target 1 kept a stale or zero blend control while
target 0 had a live one. Writing both as a two-dword `SET_CONTEXT_REG` at `0x1e0` is correct and
is what the measurement shows working.

---

## 3. Linear 3D mip levels are not 2D packing with a depth term

**Request `-9b73`.**

A 4x4x4 RGBA8 volume, level 0 red and a 2x2x2 level 1 green, sampled with `image_sample_l` at an
explicit LOD of 1.0. Three candidate layouts for where level 1 begins:

| Arm | Level 0 at | Level 1 at | Sampled |
|---|---|---|---|
| `arm1-smallest-first` | `0x400` | `0x0` | `0xff0000ff` - **red**, i.e. level 0 |
| `arm2-largest-first` | `0x0` | `0x1000` | `0x0` - unmapped |
| `arm3-slice-interleaved` | `0x200` | `0x0` | `0xff0000ff` - **red**, i.e. level 0 |

None sampled green. Two arms clamped back to the base level and one read nothing.

**What this settles.** A linear 3D image's mip chain cannot be addressed by extending the 2D rule
with a depth term - the sampler wants 3D slice and mip tile alignment that none of these three
layouts provides.

So oops-gl's **refusal of `glTexImage3D` with a mip chain stands, and is now explained rather
than merely cautious.** The earlier claim that a 3D mip chain worked was withdrawn on hardware
evidence; this is why it could not have.

Answering it properly is a new question - what the 3D tiling rule actually is - not a follow-up
to this one.

---

## 4. Five vertex parameters export and arrive intact

**Request `-4f16`.**

Programmed: `SPI_VS_OUT_CONFIG = 0x8` (`VS_EXPORT_COUNT = 4`, which is `(count-1) << 1` for five),
`SPI_PS_IN_CONTROL = 0x5` (`NUM_INTERP = 5`), and `SPI_PS_INPUT_CNTL_2..4` = `2`, `3`, `4`.
Parameter 4 carried the constant `(0.25, 0.50, 0.75, 1.00)`.

`arm3-5param-attr2` produced **`0xffbf8040`**, which is that constant byte for byte. `fence-hit
0x1`, both canaries (`0xbeef0001` VS, `0xbeef0002` PS) written, 512 pixels modified.

**A five-parameter export retires on this part and parameter 4 arrives intact.** The four-parameter
ceiling oops-gl has been working under is not a hardware limit.

**And the boundary is sharp.** Interpolating beyond the exported count stalls the pipeline - the
sweep reports `CB Busy, SX Busy, SPI Busy, PA Busy` - so `NUM_INTERP` and `VS_EXPORT_COUNT` have
to agree exactly. A fifth parameter is available; a fifth *interpolant* against a four-parameter
export is a hang, not a wrong colour.

---

## Provenance

All four resolved from obSCEne sweep `20260921-run17`, rows 10087-10502, on firmware 12.40. The
values quoted above were compared against those rows before this file was written - a resolution
summary has disagreed with its own log here before.
