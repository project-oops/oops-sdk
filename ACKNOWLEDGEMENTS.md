# Acknowledgements

oops-sdk is a clean-room re-implementation. It contains no code from any of the projects
below. They are credited as **references that pointed at a capability or documented an
interface** — the direction, never the source. Every declaration in this SDK comes from
public interface documentation, an open-source toolchain, or first-party measurement by
[obSCEne](../obscene) on real hardware; where a fact is confirmed by an obSCEne run, that
run is the citable source and carries `OBS_FROM_HARDWARE` provenance.

## Hardware media decode

The `videodec` and `audiodec` subsystems were prompted by two independent research writeups.
Both are GPL-3.0, so no code from either could be used regardless; they are credited for
identifying which platform libraries provide the capability and for documenting the shape of
the pipeline. The symbol set each subsystem binds was **confirmed independently** by
obSCEne's `107-videodec` and `108-audiodec` census on hardware.

- **ps5-hardware-video-decoding-research** — identified `libSceVideodec2` as the
  decode-into-caller-owned-GPU-memory path (zero-copy to the display), and documented the
  codec coverage (H.264 / HEVC / VP9 / HDR).
- **ps5-audio-decoding-research** — identified `libSceAudiodec` and the `libSceAjm` offload
  engine beneath it for AAC/MP3, and the microphone-capture and Opus boundaries.

## Interface documentation

- Open PlayStation homebrew toolchains and interface documentation (OpenOrbis, the
  ps5-payload-dev SDK) — for the ABI shape of platform libraries this SDK binds, on the same
  footing obSCEne's `platform.h` uses them.
- The public AMD RDNA ISA reference guides and the PM4 packet documentation the open-source
  Linux graphics stack carries (Mesa: RADV and radeonsi, and the AMDGPU LLVM backend) — for
  the register field positions, the image and sampler descriptor layouts, and the packet
  formats (`RELEASE_MEM`, `DMA_DATA`, `WAIT_REG_MEM`) the GL hardware path programs, and for the
  NGG primitive-shader protocol the vertex shader follows: `GS_ALLOC_REQ` with the vertex and
  primitive counts in m0, the primitive export word, and the export-counter wait before the exec
  mask changes (Mesa's `ac_nir_lower_ngg` emits the same sequence; the ISA reference states
  the ordering rules). Each value taken from them was then confirmed on the console;
  `docs/hardware/` holds the records and D005 the provenance decision on the NGG facts.

## Decoupled sandbox namespace service (D004)

The `oops_system_escape_sandbox()` API and the `sandbox-daemon` background service follow
architectural patterns from open-source console research projects. These projects are credited
for the credential / vnode manipulation approach and the FreeBSD kernel structure layouts.

The original trigger-file polling mechanism (`/download0/etahen_jailbreak`) was replaced
with a direct loopback TCP IPC service (`127.0.0.1:9069`) to address Prospero FW 12.40
constraints where native Big App sandboxes do not mount `/download0`. Dynamic `allproc`
discovery via kernel memory scanning (adapted from `pltauth-patch`) replaced static
heuristic offsets for reliable process resolution.

- **LightningMods/etaHEN** (`https://github.com/LightningMods/etaHEN`) — the original
  trigger-file mechanism and credential elevation pattern. The loopback IPC service
  supersedes the file-based handshake for FW 12.40+ compatibility.
- **ArkSama/PS5-Lapy-JB-Daemon** (`https://github.com/ArkSama/PS5-Lapy-JB-Daemon`) —
  the clean-room standalone breakout for consoles running `kstuff`. Shows the minimal
  PID-parse, descriptor-update, and trigger-deletion pattern.
- **pltauth-patch** (`oops-apps/src/pltauth-patch`) — dynamic allproc discovery via
  `[kdata+0x2600000, kdata+0x2B00000]` kernel memory scanning, confirmed working on
  FW 12.40.
- **FreeBSD kernel** (`sys/sys/proc.h`, `sys/sys/ucred.h`, `sys/sys/jail.h`) — the structure
  layout offsets for `struct proc` (`p_fd`, `p_ucred`, `p_pid`), `struct filedesc`
  (`fd_rdir`, `fd_jdir`), and `struct ucred` (`cr_prison`, `cr_uid`, `cr_sceauthid`,
  `cr_scecaps`) used in the credential elevation and vnode redirection.

## Web Stack Components

- **QuickJS** (Fabrice Bellard and Charlie Gordon) — MIT License. Vendored in `src/js/quickjs`
  for `oops/js.h` and `oops/webview.h`. Provides the standalone, embeddable ECMAScript 2020 engine.
- **litehtml** (Boris Rasin and contributors) — 3-Clause BSD License. Vendored in `src/html/litehtml`
  for `oops/html.h` and `oops/webview.h`. Provides the standalone HTML5 layout and CSS formatting engine.
- **Gumbo Parser** (Google Inc.) — Apache License 2.0. Vendored in `src/html/litehtml/src/gumbo`
  as the compliant HTML5 tokenizer and tree builder for litehtml.
