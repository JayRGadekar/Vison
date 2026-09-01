# Setting up Google sign-in

**You do not need any of this to build or run Vison.** Sign-in is optional -
generation, history and everything else work signed out, and a build with no
OAuth credentials simply has no **Sign in** item in its account menu. Follow
this only if you want to offer sign-in in a build you distribute.

One-time setup. Produces the client ID **and client secret** that `npm run
build` bakes into the installer. Free.

A note on that secret, because Google's naming is misleading: Google requires a
`client_secret` at the token endpoint even for "Desktop app" clients, which is a
departure from RFC 8252 and from most other providers. It therefore has to ship
inside the installer, where anyone can read it out of the asar. It is not a
credential you can protect and should not be treated as one. PKCE is what
actually secures this flow — the verifier never leaves the app process, so an
intercepted authorization code is useless without it.

Everything below happens at <https://console.cloud.google.com>.

## 1. Create a project

Project picker in the top bar → **New project**. Name it `Vison`. Any existing
project works too; the client ID is what matters, not the project.

## 2. Configure the consent screen

Left nav → **APIs & Services → OAuth consent screen**. Newer consoles put this
under **Google Auth Platform** with the fields split across *Branding*,
*Audience*, and *Data access* — same information either way.

- **User type: External.** (Internal only exists on Google Workspace and would
  restrict sign-in to your own organisation.)
- **App name:** `Vison` — this is what users see on the Google consent screen.
- **User support email** and **Developer contact email:** your own address.
- App domain / homepage / privacy policy fields: leave blank. They are only
  required for apps that need Google verification, and this one does not — see
  step 3.

## 3. Scopes — leave them alone

Vison requests `openid email profile` (`SCOPES` in `app/electron/auth.ts:25`).
These are Google's *non-sensitive* scopes: enough to show who is signed in, and
nothing more. Non-sensitive scopes need no Google security review, so there is
no verification queue to wait in and users get an ordinary consent screen rather
than an "unverified app" warning.

If you ever add a scope that touches user data — Drive, Gmail, Calendar — that
stops being true and verification becomes mandatory. Worth knowing before
anyone reaches for one.

## 4. Publish to production

On the consent screen / Audience page: **Publish app**, and confirm.

Do not skip this. In *Testing* mode Google caps you at 100 named test users and,
worse, **expires refresh tokens after 7 days**. Vison asks for offline access
and stores a refresh token (`access_type: 'offline'`, `auth.ts:210`), so leaving
the app in Testing would silently sign every user out once a week.

With only non-sensitive scopes, publishing takes effect immediately — there is
nothing to submit and no review.

## 5. Create the client ID

**APIs & Services → Credentials → Create credentials → OAuth client ID.**

- **Application type: Desktop app.** This is the one choice that will break
  things if you get it wrong. Vison binds an ephemeral loopback port and builds
  its redirect URI from whatever port the OS handed out
  (`http://127.0.0.1:<port>`, `auth.ts:201`). Desktop clients accept any
  loopback port without registering it — that is RFC 8252 behaviour, and it is
  why there is no redirect URI to fill in here.
- **Name:** anything; it is only a label in the console.

Copy **both** values from the dialog:

```
Client ID:     123456789012-abcdefghijklmnopqrstuvwxyz012345.apps.googleusercontent.com
Client secret: GOCSPX-xxxxxxxxxxxxxxxxxxxxxxxxxxx
```

You need the secret too. Skipping it is the single most likely way to get a
build that looks configured, opens the browser, collects consent, and then fails
on the last step — see the failure table below.

## 6. Build with it

```powershell
cd app
$env:VISON_GOOGLE_CLIENT_ID     = "<paste the client ID>"
$env:VISON_GOOGLE_CLIENT_SECRET = "<paste the client secret>"
npm run build
```

Watch for this line early in the output:

```
[auth] client ID baked in: 123456789012-....apps.googleusercontent.com
[auth] client secret baked in: GOCSPX-xxxxx...
```

To avoid setting both every time, persist them for your user account:

```powershell
[Environment]::SetEnvironmentVariable('VISON_GOOGLE_CLIENT_ID','<id>','User')
[Environment]::SetEnvironmentVariable('VISON_GOOGLE_CLIENT_SECRET','<secret>','User')
```

New terminals pick those up; existing ones do not. `auth.ts` prefers the runtime
value over the baked one, so this also fixes `npm run dev`, which rebuilds
`main.js` on every launch and would otherwise wipe a baked-in value.

If instead the build stops with *"Refusing to package: no Google OAuth client
ID and client secret baked into main.js"*, the environment variables were not
set in the shell that ran the build. `$env:` assignments only last for that
terminal session.

## 7. Verify the round trip

Install and run `app/release/Vison Setup 0.1.0.exe`, then click **Continue with
Google**. Expect: your default browser opens, Google asks you to pick an account
and to grant `See your primary email address` / `See your personal info`, the
tab reports that it can be closed, and the app lands on the main screen with
your name and avatar top-right.

Close the app and reopen it. You should still be signed in — that is the stored
refresh token working, and it is the part that silently breaks if step 4 was
skipped.

### If it fails

| What you see | Cause |
|---|---|
| `client_secret is missing` (400 from `/token`, after the browser flow succeeds) | No client secret in the build. Google requires one even for Desktop app clients, so this is **not** a sign that the client type is wrong — set `VISON_GOOGLE_CLIENT_SECRET` and rebuild (step 6). |
| `redirect_uri_mismatch` | Client type is **Web application**, not Desktop app (step 5). Create a new client; the type cannot be changed. |
| `invalid_client` / `Unauthorized` | The ID and the secret are from different clients. They are issued as a pair; copy both from the same one. |
| `access_blocked` / "app has not completed verification" | Consent screen is incomplete, or a sensitive scope crept in (step 3). |
| Signed out again after a week | App left in Testing mode (step 4). |
| No **Sign in** item in the account menu | The build carries no OAuth credentials. Expected for a fresh clone; rebuild with both values to offer sign-in. |

## 8. Give CI the same credentials

The workflow in `.github/workflows/build.yml` builds installers too, and it
cannot read your shell. Set both values once in the repository, under
**Settings → Secrets and variables → Actions**:

| Where | Name | Value |
|---|---|---|
| Variables | `VISON_GOOGLE_CLIENT_ID` | the client ID |
| Secrets | `VISON_GOOGLE_CLIENT_SECRET` | the client secret |

Neither is genuinely confidential — both ship inside every installer and can be
read out of the asar by anyone who unpacks one. The split above is about where
people expect to find them, not about protection: masking the ID would only make
a failed build log harder to read, and leaving the secret unmasked invites a
confusing conversation the first time someone spots it in a log. The workflow
accepts either source for either value.

Until it is set, CI still builds on every push and the installer is named
`Vison-Setup-windows-no-signin`. That is a working build, not a broken one -
it just has no sign-in. Releases are not blocked on these being set.

## Making it your own later

To rotate the credentials, create a new client and rebuild with both values — nothing in the app is
pinned to a particular one. To develop without rebuilding, set
`VISON_GOOGLE_CLIENT_ID` in your shell before launching; the runtime value wins
over the baked one (`getClientId()`, `auth.ts:52`).
