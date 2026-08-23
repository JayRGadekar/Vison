# Building and shipping Vison

## Backend

```
cmake --build build --config Release --target vison_server
```

Produces `build/bin/vison_server.exe` plus the ggml/visioncpp DLLs beside it.
The installer picks up exactly those (`app/package.json` → `build.extraResources`).

## App and installer

```
cd app
VISON_GOOGLE_CLIENT_ID="<your-id>.apps.googleusercontent.com" npx vite build
npx electron-builder --win
```

Output: `app/release/Vison Setup 0.1.0.exe`.

### The client ID must be set at build time

`auth.ts` prefers `process.env.VISON_GOOGLE_CLIENT_ID` at runtime and falls back
to a value baked in by `vite.config.ts`. Runtime env works for development, but
nobody sets a system environment variable to launch a desktop app — so a build
intended for distribution **must** have `VISON_GOOGLE_CLIENT_ID` set when
`vite build` runs, or every user gets the "Sign-in is not configured" screen and
the app is unusable.

Step-by-step setup is in [OAUTH-SETUP.md](OAUTH-SETUP.md). In short: create
the ID in Google Cloud Console → Credentials → OAuth client ID →
**Desktop app**. It is a public identifier; a desktop OAuth client has no secret,
which is why this uses PKCE.

Verify a build carries it:

```
grep -c "apps.googleusercontent.com" app/dist-electron/main.js
```

Packaging enforces this. `scripts/check-auth-configured.cjs` runs in
`beforePack` and fails the build if no client ID is present in the shipped
main-process bundle. It inspects the bundle rather than the environment on
purpose: `vite-plugin-electron` runs a separate build for the main process, so
a `define` can reach the renderer and miss `main.js` — which is how this shipped
broken once. Checking `process.env` would have passed that day.

To build a deliberately unusable installer for local testing, set
`VISON_ALLOW_UNCONFIGURED_AUTH=1`.

### Looking at the UI in a browser

`npm run dev` runs vite-plugin-electron, which launches Electron, which spawns
its own backend with a freshly generated bearer token. A browser pointed at that
dev server can therefore never reach the API, and the sign-in gate has nothing
to sign in through.

`app/vite.uicheck.config.ts` is a renderer-only config for exactly this: no
Electron plugin, so `window.vison` is undefined, the gate is skipped and `fetch`
talks straight to a backend you started yourself.

```
./build/bin/vison_server.exe          # no VISON_API_TOKEN set
cd app && npx vite --config vite.uicheck.config.ts
```

Then open <http://localhost:5199>, or screenshot it headless:

```
chrome --headless=new --window-size=1400,1200 --virtual-time-budget=15000 \
       --screenshot=out.png http://localhost:5199/
```

## Code signing

The installer is currently **unsigned**. Confirm with:

```powershell
Get-AuthenticodeSignature "app/release/Vison Setup 0.1.0.exe" | Select-Object Status
```

An unsigned installer means every user meets Windows SmartScreen's
"Windows protected your PC" dialog, where continuing is behind a "More info"
link. For a paid product that is a meaningful share of installs lost, and it
gets worse the fewer downloads the binary has — SmartScreen's reputation is
per-signature, so an unsigned build never accumulates one.

Fixing it needs a certificate, which is a purchase, not a code change:

- **OV** (organisation validated) — cheaper, but reputation still has to be
  earned over time and downloads.
- **EV** (extended validation) — SmartScreen trusts it immediately. Requires a
  hardware token or a cloud HSM, so CI signing needs a service that supports it
  (Azure Trusted Signing, SSL.com eSigner, DigiCert KeyLocker).

Once purchased, electron-builder picks it up from `win.certificateFile` +
`CSC_KEY_PASSWORD`, or from a signing service via `win.signtoolOptions`. Until
then the `signing with signtool.exe` lines in the build log are electron-builder
reporting the step, not evidence anything was signed.

## ffmpeg

Video generation muxes frames into a video by shelling out to ffmpeg. The search
order is:

1. `VISON_FFMPEG` (explicit override)
2. **beside `vison_server.exe`** — `ffmpeg.exe`, `bin/ffmpeg.exe`, `ffmpeg/ffmpeg.exe`
3. well-known install prefixes (`C:/ffmpeg/…`, `/usr/bin/…`, …)
4. `PATH`

Without a muxer the pipeline still runs and keeps the numbered PNG frames, and
the UI warns before generation starts (`/api/system` reports `ffmpeg`).

### Output is VP9 in WebM, and that is a licensing decision

The encoder is chosen at runtime from what the available ffmpeg actually has:

| Preference | Encoder | Container | Licence |
|---|---|---|---|
| 1 | `libvpx-vp9` | `.webm` | **BSD** — libvpx; VP9 is royalty-free |
| 2 | `libvpx` (VP8) | `.webm` | **BSD** |
| 3 | `mpeg4` | `.mp4` | LGPL, native to ffmpeg |

This used to be `libx264` in `.mp4`, which forced two problems at once:
libx264 is **GPL**, so any bundled ffmpeg had to be a GPL build, and H.264 sits
in the Via LA patent pool, which is a live question for a paid product that
distributes an encoder. VP9 has neither problem.

**A plain LGPL ffmpeg with `--enable-libvpx` is therefore safe to bundle, and
this build bundles one.** `build/bin/ffmpeg.exe` is
`ffmpeg-master-latest-win64-lgpl` from BtbN's FFmpeg-Builds: LGPL v3+, with
libvpx, no GPL components. It ships to `resources/backend/` beside the server,
where step 2 of the search order finds it — verified, it beats a system install
at `C:/ffmpeg`.

To replace it, drop a different `ffmpeg.exe` into `build/bin/` and rebuild. The
licence gate checks it and the notices regenerate against the new binary,
including its source-commit link. Do *not* ship an `--enable-nonfree` build;
those cannot be redistributed at all.

Obligations for an LGPL build: include ffmpeg's licence text, and make its source
available (a link to the exact release you built from is normally accepted).

### The build refuses to ship a GPL ffmpeg

`scripts/check-ffmpeg-license.cjs` runs as an electron-builder `beforePack`
hook, so it cannot be skipped by invoking `electron-builder` directly. If
`build/bin/ffmpeg.exe` exists it is interrogated with `-version` and the build
aborts on `--enable-gpl` or `--enable-nonfree`. Missing `--enable-libvpx` is a
warning rather than an error, because the MPEG-4 fallback is still licence-clean
— just worse.

Run it on its own with:

```
cd app && npm run check:ffmpeg
```

Bundling nothing is a pass: the app finds a system ffmpeg and warns when there
is none.

> Not legal advice. For a commercial release, have someone qualified confirm
> the build configuration you actually ship.

### Why WebM is fine here

The renderer is Chromium, which decodes VP9 natively, so clips play in the app
with no extra codec. Measured on an identical prompt, seed, and settings, a
480×832 clip came out at 45.8 KB as VP9 against 47.0 KB as H.264, with no
meaningful difference in encode time. The trade-off is portability: `.webm` is
less convenient than `.mp4` if a file gets dragged into another editor.

## Third-party notices

`scripts/collect-licenses.cjs` generates
`build-resources/licenses/THIRD-PARTY-NOTICES.txt` from the licence files
actually vendored in the tree, and electron-builder copies it to
`resources/licenses/` in the installed app. It runs automatically during
packaging (after the ffmpeg licence gate); to run it alone:

```
cd app && npm run licenses
```

Every component is permissive — MIT, BSD, or public domain — and they all carry
the same obligation: reproduce the copyright notice and licence text in the
distribution. Generating rather than hand-maintaining the list matters because a
hand-written one goes stale the first time a dependency is added; a missing
licence file is a hard error rather than a silent omission.

The app reads the file over the `app:notices` IPC channel and shows it under
**Model Library → Third-party licences**. Attribution that ships but cannot be
reached does not satisfy the obligation.

FFmpeg gets a notice **only when a binary is actually staged** in `build/bin`.
Everything in that notice is derived from the binary itself rather than assumed:
the LGPL version comes from its `--enable-version3` flag, and the corresponding-
source link is pinned to the git commit embedded in its version banner
(`N-126229-gf101fce22d-…` → `github.com/FFmpeg/FFmpeg/commit/f101fce22d`).
A rolling `/latest` release link would stop pointing at the shipped build the
next time upstream published, which is why it is not used.

`FFMPEG-LICENSE.txt` (the full LGPL v3 text, from the upstream archive) ships in
the same folder, and the notice points at it.

## Continuous integration

`.github/workflows/build.yml` builds the backend and packages the installer on
`windows-latest` — for pushes and PRs against `main`, on demand, and on a
published release.

It is Windows-only on purpose. `extraResources` in `app/package.json` filters
the backend down to `vison_server.exe`, `*.dll` and `ffmpeg.exe`, none of which
a macOS or Linux build produces, so a mac job would go green and hand back a
`.dmg` with no backend inside it. Those targets are real work, not a matrix
entry.

Three things it has to do that a local build gets for free:

- **Restore `third_party/`.** It is not in the repo, so the first real step is
  `scripts/bootstrap-third-party.sh` (see [PATCHES.md](../PATCHES.md)). Cached,
  keyed on that script and the patch files.
- **Install the Vulkan SDK**, not just the runtime. `glslc` ships only in the
  SDK, and without it the root `CMakeLists.txt` quietly falls back to CPU. CI
  passes `-DVISON_VULKAN=ON` explicitly so a missing SDK is a build failure
  rather than a green run producing a backend nobody wants, then asserts
  `ggml-vulkan.dll` actually landed in `build/bin`.
- **Download the LGPL ffmpeg** from BtbN and stage it into `build/bin` before
  packaging, so the bundled binary is the one the licence gate expects.

It also smoke-tests the backend: start `vison_server.exe`, wait for
`/api/system`, stop it. That is aimed at the failure mode with no error message
at all — a DLL that cannot be resolved and a process that simply vanishes.

### CI needs the client ID too

Set **`VISON_GOOGLE_CLIENT_ID`** as a repository *variable* (Settings → Secrets
and variables → Actions → Variables). It is a public identifier — a desktop
OAuth client has no secret — so a variable is the honest home for it; a secret
of the same name is read as a fallback.

Until it is set, push and PR builds still run. They use
`VISON_ALLOW_UNCONFIGURED_AUTH=1`, and the artifact is named
`Vison-Setup-windows-UNCONFIGURED` because that installer shows "Sign-in is not
configured" and can do nothing else. A **published release** with no client ID
fails instead, which is the one case where the unusable build would actually
reach users.

## Environment variables the backend reads

| Variable | Effect |
|---|---|
| `VISON_DATA_DIR` | Root for `models/` and `outputs/`. The launcher sets this to per-user storage; without it, paths are relative to the working directory. |
| `VISON_API_TOKEN` | When set, `/api/*` requires `Authorization: Bearer <token>`. The Electron main process mints one per launch. Unset = open (development). |
| `VISON_FFMPEG` | Explicit path to the muxer. |
| `VISON_BACKEND_SPEC`, `VISON_MAX_VRAM`, `VISON_VRAM_PROFILE` | Override the automatic backend plan. |
| `VISON_GOOGLE_CLIENT_ID` | OAuth client ID; overrides the baked-in value. |
