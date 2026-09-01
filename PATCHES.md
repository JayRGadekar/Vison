# Local patches to vendored dependencies

`third_party/` holds **full git clones**, not submodules — there is no `.gitmodules`,
and the outer repo does not track their contents (`.gitignore` excludes them, because
git could only record them as gitlinks pointing at upstream commits that carry none of
the changes below). **A fresh clone of Vison therefore has no `third_party/` sources at
all**, and re-cloning or hard-resetting a dependency silently reverts these patches.

To rebuild it:

```sh
sh scripts/bootstrap-third-party.sh
```

That reclones both dependencies at the pinned commits and applies
`third_party/patches/`, which is checked in and does reproduce the working tree. The
patch file is verified to apply cleanly to a pristine `db99efd` checkout. Re-running the
script is harmless: it skips clones that already exist and patches already applied.

Keep the patch file and the prose below in step — if you change one, change the other.

At least one of these patches is load-bearing: without it FLUX will not load at all.
If you ever refresh a dependency, re-apply its patches and re-check this file.

To see the current state of a dependency:

```sh
cd third_party/<dep> && git status --short && git diff --ignore-cr-at-eol --ignore-all-space
```

---

## third_party/stable-diffusion.cpp

Upstream: <https://github.com/leejet/stable-diffusion.cpp.git>
Pinned at: `487de75` ("fix: fail with a message when MiniMax-H3 is run in img_gen mode (#1863)"), branch `master`

Bumped from `db99efd` (Aug 2) on 2026-08-30 to pick up upstream's native
MiniMax-H3 support (`ea7f0c8` and its stabilizing follow-ups through
`487de75`), needed to add that model to Vison's registry.

**Deliberately not bumped further to current upstream HEAD.** The very next
commit, `bcc7e29` ("feat: support INT8 ConvRot safetensors"), adds calls to
`ggml_mul_mat_i8_tensorwise` / `ggml_quantize_i8_convrot` and the
`GGML_TYPE_F8_E4M3`/`GGML_TYPE_F8_E5M2` enum values to `src/core/ggml_extend.hpp`
that do not exist in the `ggml` commit this project actually links. That is
not an isolated version skew: `third_party/vision.cpp` bundles its *own* fork
of ggml (`depend/llama/ggml`, currently `5fde0fae4`, with `[VISION]`-prefixed
Vulkan-op commits of its own), and `third_party/cmakelists.txt` adds
`vision.cpp` before `stable-diffusion.cpp` (`add_subdirectory(vision.cpp)`
then `add_subdirectory(stable-diffusion.cpp)`). Both vendor a CMake target
literally named `ggml`; stable-diffusion.cpp's own `CMakeLists.txt` only adds
its (newer) `ggml` submodule `if (NOT TARGET ggml)`, so whichever
subdirectory runs first wins the shared target for the whole build - which is
vision.cpp's older fork today. The two ggml copies cannot simply be
reconciled by bumping one of them: they are independently-maintained forks
that have each added their own backend-specific ops, and only one `ggml`
target can exist in a single link unit (two independent definitions of e.g.
`ggml_mul_mat` would be an ODR/link conflict in the final `vison_server`).
Any future bump past `487de75` needs one of: updating vision.cpp's bundled
ggml fork to a version that is a superset of both forks' additions, or a
different resolution of the shared-target collision - not just moving the
stable-diffusion.cpp pin forward. `1706b32` (upstream's own fix for the VRAM
clamp bug, patch 2 below) landed after this pin for the same reason: it is
not included here, so patch 2 is still carried locally.

### 1. `src/model_loader.cpp` — recognise bare FLUX tensor names **(load-bearing)**

`ModelLoader::get_sd_version()` only detected FLUX from tensor names prefixed with
`model.diffusion_model.`. city96's `FLUX.1-schnell-gguf` — the file the model registry
in `server/src/server.cpp` downloads — names its tensors bare (`double_blocks.0...`,
`single_blocks.0...`), so detection failed and the model was rejected as an unknown
architecture.

```diff
         if (tensor_storage.name.find("model.diffusion_model.double_blocks.") != std::string::npos ||
-            tensor_storage.name.find("model.diffusion_model.single_transformer_blocks.") != std::string::npos) {
+            tensor_storage.name.find("model.diffusion_model.single_transformer_blocks.") != std::string::npos ||
+            tensor_storage.name.find("double_blocks.") == 0 ||
+            tensor_storage.name.find("single_blocks.") == 0) {
             is_flux = true;
         }
```

**Remove this and image generation stops working.**

### 2. `src/core/ggml_extend.hpp` — let the VRAM clamp actually hold

In `resolve_graph_cut_plan()`, the streaming budget is correctly clamped to the driver's
reported free VRAM (minus a 512 MB margin) — and then a high-water-mark ratchet a few
lines later restores the largest budget ever observed, discarding the clamp. The budget
becomes monotonically non-decreasing. Logs show it directly:

```
clamping streaming budget: actual free VRAM 2290MB < user cap 4096MB
streaming budget = 4096.00 MB          <-- clamp discarded
```

A budget larger than free VRAM makes `annotate_residency()`
(`src/core/ggml_graph_cut.cpp:963`) promote more segments to `RESIDENT` than the card can
hold. The allocation fails inside ggml and **aborts the process** — not a C++ exception,
so nothing in `core/src/queue.cpp` can catch it; the backend simply disappears. This is
what `app/electron/main.ts`'s bounded backend auto-restart exists to survive.

The patch adds a `budget_clamped` flag and, when the clamp fired, pulls the high-water
mark down instead of overriding the clamp. The mark still suppresses oscillation from
noisy readings in every other case.

**Measured effect** (RTX 4050 6GB, FLUX schnell 4 steps, `VISON_MAX_VRAM=4`):

The budget now moves in both directions instead of latching high — observed values were
4096.00 MB when VRAM was genuinely free, clamping to 2390.77 / 2290.89 / 2290.70 MB under
pressure, with segment counts of 13 / 30 / 57 / 59 to match. Equilibrium is ~2.3 GiB.

| config | 1024x576 sampling | 1024x1024 sampling | safe |
|---|---|---|---|
| `max_vram=2` (default) | 79-91s | 104s | yes |
| `max_vram=4`, unpatched | 45s | 83s | **no** - aborted the process once |
| `max_vram=4`, patched | 65-67s | 104s | yes - 9 clamps, 0 aborts |

So the patch does **not** recover the unpatched 4 GiB speed: that came from over-committing.
It buys roughly 20-25% at 1024x576 (early graphs legitimately have VRAM spare) and nothing
at 1024x1024. Its real value is turning a silent process abort into a graceful slowdown.

**This patch is what makes automatic VRAM planning possible.** `plan_backend()` in
`core/src/device.cpp` now derives `max_vram` from the device's real free VRAM instead of
using a hardcoded 2 GiB, which on this 6GB card yields 4 GiB and ~25% faster sampling
(62.3s vs 79-91s at 1024x576). That is only safe because the clamp below actually holds:
without the patch, a derived budget would over-commit and abort the process.

Residual caveat: when the clamp lowers the budget, the cached plan's *segment sizes* were
chosen under the earlier larger `planner_budget`. `annotate_residency()` recomputes
residency correctly, but the segments themselves may be oversized. No aborts were observed,
but this is not proven safe.

Upstream fixed the same underlying bug independently in `1706b32` ("fix: re-clamp
streaming VRAM budget to currently free memory (#1878)"), with an equivalent effect
(`std::min(observed_max_effective_budget_, free_clamp)` instead of blindly restoring the
high-water mark) — but that commit lands after the `487de75` pin above (see the "not
bumped further" note), so it is not yet available and this patch stays. **Drop this patch
in favor of upstream's fix the next time the pin moves far enough to include `1706b32`.**

### 3. `CMakeLists.txt` — build integration

Several unrelated build changes, none behavioural:

- `GGML_MAX_NAME` lowered from upstream's `160` to `128`, to agree with the global
  `add_compile_definitions(GGML_MAX_NAME=128)` in the root `cmakelists.txt`. Every target
  must use one value or `ggml_tensor::name` sizes disagree across translation units and
  tensor names get silently truncated. **Caveat:** upstream chose 160 deliberately, so
  the safer reconciliation is raising the root to 160 rather than lowering sd.cpp to 128.
  Nothing currently loaded needs >127 chars, so this is latent, not broken.
- `zip` linked as a library (`target_link_libraries(... ggml zip)`) instead of being
  injected as `$<TARGET_OBJECTS:zip>`.
- Install rules reduced to the library plus its public header; upstream's
  `configure_package_config_file` / pkg-config generation removed.
- `/bigobj` added for MSVC — some translation units exceed the default section limit.

The file also has CRLF line endings, so a plain `git diff` shows the whole file as
changed. Use `--ignore-cr-at-eol --ignore-all-space` to see the real diff.

---

## third_party/vision.cpp

Upstream: <https://github.com/Acly/vision.cpp.git>
Pinned at: `26a7529`, branch `main`

**No local modifications.** Verified with `git status --short` (clean).

Note that `visp::image_save()` only ever writes PNG regardless of the filename — this is
upstream behaviour, not a patch. Vison works around it in
`pipelines/common/include/vison/pipelines/image_output.h` rather than modifying the
dependency.

---

## third_party/cpp-httplib, third_party/nlohmann_json

Header-only, consumed via `INTERFACE` targets in `third_party/cmakelists.txt`.

- `cpp-httplib` — a single vendored `httplib.h`, `CPPHTTPLIB_VERSION "0.52.0"`
- `nlohmann_json` — vendored `include/`, major version 3

Unlike the two above, these are **plain source drops, not git clones**, so there is no
upstream to diff against and local edits cannot be detected. If either ever needs
patching, record it here by hand — nothing else will notice.
