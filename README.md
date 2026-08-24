# Vison

Local image and video generation on your own machine. No account with a
generation service, no per-image credits, no prompts leaving your computer.

Vison is a desktop app around a C++/Vulkan backend built on
[stable-diffusion.cpp](https://github.com/leejet/stable-diffusion.cpp). You pick
a model, it downloads once, and everything after that runs on your GPU.

> **Status: early.** One developer, one machine, Windows only so far. It works
> — the models below were run on a 6 GB laptop GPU to produce real output — but
> it has not been through many hands yet. Expect rough edges, and please report
> them.

## What it does

- **Text to image**, four model tiers from a 4 GB card upward
- **Text to video**, likewise
- **Upscaling** for both, via Real-ESRGAN
- **Image to image**, by attaching a starting image
- Chat-style history of everything you have made, searchable by prompt

## Requirements

- **Windows 10/11 x64.** macOS and Linux are not built yet — see
  [Platform support](#platform-support).
- **A GPU with Vulkan drivers.** A 4 GB card runs the lighter models; 6 GB
  comfortably runs FLUX.1 Schnell. There is a CPU fallback, and it is slow
  enough that you will not want it for anything but a test.
- **Disk space for weights.** The lightest model is under 4 GB; the full set is
  around 30 GB. Nothing is downloaded until you ask for it.

Vison reads your GPU's actual VRAM and marks models you cannot run, rather than
letting you download 20 GB and then fail.

## Install

Grab the installer from [Releases](../../releases). It is currently **unsigned**,
so Windows SmartScreen will warn you — "More info" then "Run anyway", or build
from source below if you would rather not take that on trust.

## Build from source

Full detail in [docs/BUILD.md](docs/BUILD.md). The short version:

```sh
# The vendored dependencies are not in this repo - they are upstream clones
# carrying local patches. This fetches them at the pinned commits and applies
# the patches.
sh scripts/bootstrap-third-party.sh

cmake -S . -B build -A x64 -DCMAKE_BUILD_TYPE=Release -DVISON_VULKAN=ON
cmake --build build --config Release --target vison_server --parallel

cd app && npm ci && npm run build
```

You need the **Vulkan SDK**, not just the runtime — `glslc` compiles ggml's
compute shaders and only ships in the SDK.

## Models

Everything is pulled from Hugging Face on demand. The registry lives in
`server/src/server.cpp`.

| Task | Model | Size | Min VRAM |
|---|---|---|---|
| Image | SDXL Turbo | 3.8 GB | 4 GB |
| Image | Z-Image Turbo | 7.5 GB | 4 GB |
| Image | FLUX.1 Schnell | ~16 GB | 6 GB |
| Image | Qwen-Image | large | high |
| Video | Wan 2.1 T2V 1.3B | ~3 GB | 4 GB |
| Video | Wan 2.2 TI2V 5B | ~5 GB | 6 GB |
| Video | Wan 2.2 T2V A14B | very large | high |
| Video | HunyuanVideo 1.5 | very large | high |
| Upscale | Real-ESRGAN x4 | 9 MB | low |

**Being straight about coverage:** SDXL Turbo, Z-Image Turbo, FLUX.1 Schnell,
the two smaller Wan video models and Real-ESRGAN have all been run here and
produce output. Qwen-Image, Wan 2.2 A14B and HunyuanVideo 1.5 are registered
and wired up but have **never been run** — they do not fit on the development
machine. If you have the hardware, that is the single most useful thing you
could report back.

## Privacy

Everything runs locally. The backend listens on `127.0.0.1`, the weights sit on
your disk, and generated images and video are written to your own folders. No
prompt, image, or video is uploaded anywhere.

Two things do reach the network, both only when you ask: downloading model
weights from Hugging Face, and Google sign-in if it is configured.

## How it works

```
Electron + React  ->  IPC  ->  main process  ->  HTTP 127.0.0.1:11439
                                                        |
                                          vison_server.exe (C++)
                                                        |
                                  stable-diffusion.cpp + ggml (Vulkan)
```

Chat history is SQLite in your user data directory, with an FTS5 index so you
can search conversations by what you asked for rather than by title.

## Platform support

Windows only, for now, and honestly rather than by principle: the packaging
config copies `vison_server.exe` and `*.dll`, which matches nothing a macOS or
Linux build produces. The backend itself is portable C++ and Vulkan — most of
the work is packaging and testing, not porting. Help welcome.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). The one thing to know before you start:
`third_party/` is not vendored into this repo, and one of the local patches is
load-bearing — without it FLUX is rejected as an unknown architecture. Run
`scripts/bootstrap-third-party.sh` first.

Bug reports are as valuable as patches, especially on hardware other than a
6 GB NVIDIA laptop GPU.

## Support the project

Vison is free and MIT-licensed, and it will stay that way. There is no paid
tier, nothing held back, and no plan to add either.

If it is useful to you and you would like to chip in, there is a **Sponsor**
button at the top of this repository. Entirely optional — a bug report on
hardware I do not own is worth just as much.

## Licence

[MIT](LICENSE).

Vison builds on other people's work, all of it permissively licensed:
[stable-diffusion.cpp](https://github.com/leejet/stable-diffusion.cpp) and
[ggml](https://github.com/ggml-org/ggml) (MIT),
[vision.cpp](https://github.com/Acly/vision.cpp) (MIT), and others listed in
`app/build-resources/licenses/THIRD-PARTY-NOTICES.txt`, which is generated from
the licences actually present in the tree and shipped inside the app.

The bundled `ffmpeg.exe` is an **LGPL** build with libvpx and no GPL
components; the build refuses to package a GPL or non-free one. Video is
encoded as VP9 in WebM, which is royalty-free — that is a deliberate licensing
choice, not a technical one.

Model weights are **not** covered by this licence. Each carries its own terms
from whoever published it; FLUX.1 Schnell, the Wan models and the rest are all
different. Check them before using output commercially.
