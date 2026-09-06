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
