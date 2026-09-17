# Shader source for the words that ship

`oops-gl` emits RDNA2 bytecode as literal `uint32_t` words. That is the right shape for a
measuring instrument - the stream is readable as evidence, and there is no compiler between a GL
call and the packet - and it has one hazard: **a wrong instruction encoding cannot fail loudly.**
It assembles into the payload, the hardware does something else, and the result is a frame that
is wrong rather than a build that stops.

So the words are not written from memory. Each `.s` file here is assembled, and the object read
back, and the resulting words are what go into the source with the instruction in the comment
beside them:

```bash
clang -target amdgcn-amd-amdhsa -mcpu=gfx1030 -c tools/shader/alpha-test.s -o /tmp/a.o
objdump -s -j .text /tmp/a.o
```

`clang` is in the `oops-builder` WSL distribution; there is no `llvm-mc` there, but the AMDGPU
target is built in, which is all this needs.

Two of the results agree with words that were already in the tree, which is the cross-check that
the pipeline is right rather than merely self-consistent: `s_endpgm` assembles to `0xbf810000`,
and the 32-bit-literal source marker `0xff` appears in the same position as in the canary load
the untextured pixel shader has always done.

## Wave32

These shaders run wave32 - `VGT_SHADER_STAGES_EN` sets `GS_W32` and `VS_W32` - so the mask
registers are `vcc_lo` and `exec_lo`. A wave64 build would need `vcc` and `exec`, and the
encodings would differ; assembling for the wrong width is exactly the mistake this directory
exists to prevent.

## Files

- `alpha-test.s` - the comparisons and the lane kill behind `glAlphaFunc`. The words are checked
  against this file by `test_gl_alpha_test_patches_both_shaders`.
