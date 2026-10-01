# Tasks Remaining — Deploy & Market Vison

Snapshot as of 2026-09-02, based on the actual state of the repo (git log, CI
config, FUNDING.yml, README, PATCHES.md).

Each item is tagged with who has to actually do it:
**[You]** needs your accounts, money, credentials, or hardware — I can't do it.
**[Me]** is file/code/content work — ask and I'll do it now.
**[Together]** needs a decision from you, then I execute.

## Deployment

- [ ] **[Together]** Clean up uncommitted changes — `app/build-resources/licenses/THIRD-PARTY-NOTICES.txt`,
      `app/package-lock.json`, and `cmakelists.txt` currently show as modified
      but uncommitted. I can commit these once you say go (see status below).
- [ ] **[Together]** Cut an actual GitHub Release. No git tags exist yet —
      despite `.github/workflows/build.yml` listening for `release: published`,
      nothing has been published. I can tag `v0.1.0` locally and draft the
      release; pushing the tag and publishing the release on GitHub needs
      your push access / GitHub auth.
- [ ] **[You]** Decide on OAuth for the shipped build and set it up. Needs the
      Google Cloud OAuth consent screen created and **published to production**
      (docs/OAUTH-SETUP.md steps 1-4), and `VISON_GOOGLE_CLIENT_ID`/
      `VISON_GOOGLE_CLIENT_SECRET` added as GitHub repo secrets/vars — all
      account actions only you can perform.
- [ ] **[You]** Code signing certificate. Requires either buying a cert or
      applying to a program like SignPath's open-source offer, which needs
      your identity/org verification. I can wire it into `electron-builder`
      once you have one.
- [ ] **[You]** Confirm GitHub Sponsors is actually live. The KYC step (PAN,
      bank details, Stripe) is tied to your identity — only you can complete
      it or check its status.
- [ ] **[Me]** Verify the release workflow end-to-end — I can re-read
      `build.yml` and confirm it attaches the `.exe` to a published Release
      correctly (static review; an actual live run still needs step above).
- [ ] **[You]** Smoke-test the packaged installer on a clean (non-dev) machine
      — needs a second physical/VM machine, which I don't have access to.
- [ ] **[You]** Untested models (Qwen-Image, Wan 2.2 T2V A14B, HunyuanVideo
      1.5) need bigger GPU hardware than the dev machine to actually run —
      needs either your access to that hardware or a cloud GPU rental you set up.
- [ ] **[Together]** Add screenshots/GIFs to the README — I can write the
      README section and pick candidate images from `outputs/`, but capturing
      a live demo GIF needs the app running on your machine.
- [x] **[Me]** Release notes / CHANGELOG for v0.1.0 — drafted, see
      [CHANGELOG.md](CHANGELOG.md).
- [ ] **[You]** Confirm platform scope for launch (Windows-only vs. waiting on
      macOS/Linux) — this is a product call only you can make.

## Marketing

- [ ] **[Together]** Record a demo GIF/video of generation running — I can
      suggest shot list/framing, but recording your screen is on your machine.
- [x] **[Me]** Vison-specific launch posts drafted — see
      [vison_launch_posts.md](../vison_launch_posts.md) (Show HN, r/StableDiffusion,
      r/selfhosted, notes on r/LocalLLaMA fit).
- [x] **[Me]** Show HN post — included in the file above.
- [ ] **[You]** Reach out to local-AI/self-hosted reviewers — needs you to
      actually send the messages/emails from your accounts. I can draft the
      outreach template if useful.
- [ ] **[You]** Submit to OSS/AI directories (AlternativeTo, LibHunt, awesome-lists)
      — most require an account or a PR from your GitHub identity.
- [ ] **[You]** Decide on a social presence (X/Mastodon/Bluesky) — account
      creation and ongoing posting are yours; I can draft copy for any of them.
- [ ] **[Together]** Sequencing — don't post launches before the signed
      installer + Release are live. I'll flag if asked to post prematurely.
- [ ] **[You]** Follow up on Sponsors KYC before "support the project"
      messaging goes out in launch posts.

## Suggested order

1. Commit/clean working tree → 2. Test untested models or gate them → 3. Get
code signing sorted → 4. Cut the v0.1.0 GitHub Release → 5. Add
screenshots/demo GIF to README → 6. Confirm Sponsors is live → 7. Write and
post launch content.
