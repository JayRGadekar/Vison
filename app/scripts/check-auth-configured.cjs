// Refuses to package an installer whose sign-in screen is a dead end.
//
// The whole UI sits behind Google sign-in (see the gate in App.tsx). auth.ts
// reads the client ID from process.env first and falls back to a value baked
// in at build time by vite's `define` - and nobody sets a system environment
// variable to launch a desktop app, so on an installed copy the baked value is
// the only one there is. Build without VISON_GOOGLE_CLIENT_ID and every user
// gets "Sign-in is not configured" with no way past it: an installer that
// cannot do anything at all.
//
// That failure is invisible from the build log - packaging succeeds, the
// installer runs, and the app is simply useless. It has already shipped once
// silently, when the define reached the renderer but not the main-process
// build (vite-plugin-electron runs those separately).
//
// So this checks the artifact rather than the environment: the value has to be
// findable in the bundle that actually ships. Reading process.env here would
// have passed that day.

const fs = require('fs');
const path = require('path');

const BUNDLE = path.resolve(__dirname, '..', 'dist-electron', 'main.js');

// Google desktop OAuth client IDs are always <digits>-<token>.apps.googleusercontent.com.
const CLIENT_ID = /[0-9]+-[a-z0-9]+\.apps\.googleusercontent\.com/;

// ...and current client secrets are GOCSPX-<token>. Google requires one at the
// token endpoint even for "Desktop app" clients, so a build with the ID alone
// gets all the way through the browser flow and then dies on the exchange with
// "client_secret is missing" - after the user has already picked an account and
// granted consent. That is a worse first impression than not shipping at all,
// which is why it is checked here rather than left to fail at runtime.
const CLIENT_SECRET = /GOCSPX-[A-Za-z0-9_-]{10,}/;

const OVERRIDE = 'VISON_ALLOW_UNCONFIGURED_AUTH';

function checkAuthConfigured({ quiet = false } = {}) {
  if (process.env[OVERRIDE] === '1') {
    console.warn(
      `[auth] ${OVERRIDE}=1 — packaging without a client ID.\n` +
      `       Sign-in will fail in this build; it is for local testing only.`);
    return;
  }

  if (!fs.existsSync(BUNDLE)) {
    throw new Error(
      `Main-process bundle not found at ${BUNDLE}.\n` +
      `Run the renderer/main build before packaging.`);
  }

  const bundle = fs.readFileSync(BUNDLE, 'utf8');
  const id = bundle.match(CLIENT_ID);
  const secret = bundle.match(CLIENT_SECRET);

  if (id && secret) {
    if (!quiet) {
      console.log(`[auth] client ID baked in: ${id[0]}`);
      // Deliberately not printed in full. It is not a real secret - it ships in
      // the installer - but echoing it into build logs that get pasted around
      // invites a confusing conversation about whether it leaked.
      console.log(`[auth] client secret baked in: ${secret[0].slice(0, 12)}...`);
    }
    return;
  }

  const missing = [!id && 'client ID', !secret && 'client secret'].filter(Boolean).join(' and ');

  throw new Error(
    `Refusing to package: no Google OAuth ${missing} baked into ${path.basename(BUNDLE)}.\n\n` +
    `Every screen in Vison is behind sign-in, so this installer would fail for\n` +
    `every user who installs it - with no client ID it shows "Sign-in is not\n` +
    `configured", and with no client secret it dies on the token exchange after\n` +
    `the user has already granted consent.\n\n` +
    `Create an OAuth 2.0 Client ID of type "Desktop app" in Google Cloud Console\n` +
    `(APIs & Services -> Credentials), then build with both values set:\n\n` +
    `  $env:VISON_GOOGLE_CLIENT_ID = "<id>.apps.googleusercontent.com"\n` +
    `  $env:VISON_GOOGLE_CLIENT_SECRET = "GOCSPX-<...>"\n` +
    `  npm run build\n\n` +
    `Both are public identifiers despite the name on the second: Google requires\n` +
    `a client_secret even for desktop clients, so it ships inside the installer\n` +
    `where anyone can read it. PKCE is what secures the flow. To build a\n` +
    `deliberately unusable installer for local testing, set ${OVERRIDE}=1.\n` +
    `See docs/OAUTH-SETUP.md.`);
}

module.exports = checkAuthConfigured;
module.exports.checkAuthConfigured = checkAuthConfigured;

if (require.main === module) {
  try {
    checkAuthConfigured();
  } catch (err) {
    console.error(`\n[auth] ${err.message}\n`);
    process.exit(1);
  }
}
