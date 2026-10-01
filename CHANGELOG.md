# Changelog

## [0.1.0] — 2026-10-01

First public release (published as a pre-release). Windows installer:
`Vison.Setup.0.1.0.exe`, attached to the
[v0.1.0 release](https://github.com/JayRGadekar/Vison/releases/tag/v0.1.0).

Initial public version.

### Added
- Local text-to-image and text-to-video generation via a C++/Vulkan backend
  built on stable-diffusion.cpp, running on 4 GB+ GPUs.
- Four image model tiers (SDXL Turbo, Z-Image Turbo, FLUX.1 Schnell,
  Qwen-Image) and video via Wan 2.1/2.2 and HunyuanVideo 1.5.
- LTX 2.5 Distilled (video + audio) registered in the model list; untested on real hardware (about 26 GB of weights).
- "Add from Hugging Face" in the library: paste a repo or file link and Vison matches it to a supported family (FLUX, Wan, HunyuanVideo, LTX 2.5, MiniMax H3, Z-Image, Qwen-Image, SDXL, and GGUF ESRGAN upscalers), reusing that family's text encoder and VAE. Upscalers must be GGUF.
- Image-to-image generation by attaching a starting image.
- Upscaling for both images and video via Real-ESRGAN.
- Chat-style history of every generation, stored in SQLite with an FTS5
  index for full-text search over past prompts.
- Optional Google sign-in (PKCE-based); the app is fully usable signed out.
- Automatic VRAM-aware model gating — Vison reads the GPU's free VRAM and
  disables models that won't fit rather than letting a download fail later.
- Bounded auto-restart for the backend process, to recover from GPU-driven
  crashes without losing the app session.
- Open-sourced under the MIT license, with GitHub Sponsors, issue templates,
  and contribution docs.

### Known limitations
- Windows only; macOS and Linux are not packaged yet.
- The installer is unsigned, so Windows SmartScreen will warn on first run.
- Qwen-Image, Wan 2.2 T2V A14B, and HunyuanVideo 1.5 are registered and wired
  up but have not been run to completion — the development machine (6 GB
  laptop GPU) can't fit them.
