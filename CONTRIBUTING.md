# Contributing to Vison

Bug reports are as welcome as code — more so, in some cases. Vison has been
developed against exactly one GPU, so a report from different hardware tells me
something I cannot find out on my own.

## Before you build: third_party

`third_party/` is **not** in this repository. The dependencies are full upstream
clones carrying local patches, and git can only record those as gitlinks
pointing at commits that have none of our changes. So they are reconstructed
instead:

```sh
sh scripts/bootstrap-third-party.sh
```

That clones at the pinned commits and applies `third_party/patches/`. It is
idempotent — run it again any time.

**One of those patches is load-bearing.** `ModelLoader::get_sd_version()` is
patched to recognise bare FLUX tensor names; without it, city96's FLUX GGUF is
rejected as an unknown architecture and image generation with FLUX does not
work at all. If you find yourself wondering why a fresh clone cannot load a
model, this is why. See [PATCHES.md](PATCHES.md).

## Building

[docs/BUILD.md](docs/BUILD.md) has the full story. You need the **Vulkan SDK**,
not just the runtime: `glslc` compiles ggml's shaders and only ships in the SDK.
Without it the build silently falls back to CPU, which is not what you want.

## Tests

```sh
cd app && npm test
```

There is no test framework and adding one is not the goal. Node 24 imports
TypeScript directly, so tests are plain `node:assert` scripts. They cover the
places where a bug costs the user something they cannot get back — chat history
storage, the search index, conversation titles.

If you change `app/electron/chat-store.ts`, run these. What is at risk there is
someone's conversation history.

## Changing vendored code

Patch the tree, then regenerate the patch file so a fresh clone reproduces what
you have:

```sh
cd third_party/stable-diffusion.cpp
git diff --ignore-cr-at-eol -- <files> > ../patches/0001-sd-cpp-vison.patch
```

Then check it applies both ways — cleanly onto a pristine checkout, and in
reverse against your tree. Keep `PATCHES.md` in step with what the patch
actually does; prose that has drifted from the code is worse than no prose.

Better still: if a change belongs upstream, send it there. Two known candidates
are the `resolve_graph_cut_plan()` high-water-mark ratchet that discards the
VRAM clamp, and `VK_CHECK` in ggml-vulkan calling `exit(1)` inside a library.

## Style

Match the surrounding code — it is fairly consistent about this. Comments
explain *why*, especially where something looks odd: nearly every strange-looking
line in this codebase is load-bearing, and the comment above it says what broke
when it was not there. If you fix something subtle, leave that note behind for
the next person.

## Sending a change

Fork, branch, PR against `main`. CI builds the backend and the installer on
Windows; it takes a while, because compiling ggml's Vulkan shaders is most of
the run.

Small PRs get looked at faster than large ones. If you are planning something
big, open an issue first so we do not both write it.

## Licence

Contributions are under the [MIT licence](LICENSE), same as the rest.
