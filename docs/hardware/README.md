# `docs/hardware/` - what the console answered

Measurements taken on the real target by programs built from this SDK, kept in the tree because
a hardware run cannot be reproduced from source by anyone without the console. Each file names
the build, the console state and the date, and quotes the records unedited, so a claim in a
comment or a decision can be traced to a line here. The convention is the one obSCEne uses for
its own `data/hardware/`.

| | |
|---|---|
| `agc-gl-cube-oracle-fw1240.md` | the GL cube frame on firmware 12.40: the whole command stream, shaders, descriptors, fence, GPU clock and pixel hash, untextured and textured |
| `agc-blend-and-export-fw1240.md` | four answers from obSCEne sweep `20260921-run17`: a constant-colour blend reads green from `CB_BLEND_ALPHA`, dual-target blending works, linear 3D mip levels are not 2D packing with a depth term, and a fifth vertex parameter exports intact |
