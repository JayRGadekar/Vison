# Setting up Google sign-in

One-time setup. Produces the client ID that `npm run build` bakes into the
installer. Free, and there is no client secret to look after — desktop OAuth
clients do not have one, which is why `app/electron/auth.ts` uses PKCE.

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
  why there is no redirect URI to fill in here. A **Web application** client
  demands an exact registered URI, which a random port can never match, and
  every sign-in fails with `redirect_uri_mismatch`.
- **Name:** anything; it is only a label in the console.

Copy the client ID. It looks like:

```
123456789012-abcdefghijklmnopqrstuvwxyz012345.apps.googleusercontent.com
```

If the dialog also shows a client secret, ignore it. Vison does not use one and
must not ship one.

## 6. Build with it

```powershell
cd D:\Project\Vllama\app
$env:VISON_GOOGLE_CLIENT_ID = "<paste the client ID>"
npm run build
```

Watch for this line early in the output:

```
[auth] client ID baked in: 123456789012-....apps.googleusercontent.com
```

If instead the build stops with *"Refusing to package: no Google OAuth client
ID is baked into main.js"*, the environment variable was not set in the shell
that ran the build. `$env:` assignments only last for that terminal session.

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
| `redirect_uri_mismatch` | Client type is Web application, not Desktop app (step 5). Create a new client; the type cannot be changed. |
| `access_blocked` / "app has not completed verification" | Consent screen is incomplete, or a sensitive scope crept in (step 3). |
| Signed out again after a week | App left in Testing mode (step 4). |
| "Sign-in is not configured" | No client ID in the build — this build predates the packaging gate, or `VISON_ALLOW_UNCONFIGURED_AUTH=1` was set. |

## 8. Give CI the same client ID

The workflow in `.github/workflows/build.yml` builds installers too, and it
cannot read your shell. Set the ID once in the repository:

**Settings → Secrets and variables → Actions → Variables → New repository
variable**, named `VISON_GOOGLE_CLIENT_ID`, with the same value.

A *variable*, not a secret. A client ID is a public identifier — it ships inside
every installer and is readable by anyone who unpacks one — and masking it in
the logs only makes a failed build harder to read. The workflow does check
`secrets.VISON_GOOGLE_CLIENT_ID` as a fallback if you prefer it there anyway.

Until it is set, CI still builds on every push, but the installer is named
`Vison-Setup-windows-UNCONFIGURED` and shows "Sign-in is not configured" —
useful as a compile check, useless to a user. Publishing a **release** without
it fails the build outright, which is the one case where an unusable installer
would reach people who trust it.

## Making it your own later

To rotate the ID, create a new client and rebuild — nothing in the app is
pinned to a particular one. To develop without rebuilding, set
`VISON_GOOGLE_CLIENT_ID` in your shell before launching; the runtime value wins
over the baked one (`getClientId()`, `auth.ts:52`).
