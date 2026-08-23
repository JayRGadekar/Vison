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
  const found = bundle.match(CLIENT_ID);
  if (found) {
    if (!quiet) console.log(`[auth] client ID baked in: ${found[0]}`);
    return;
  }

  throw new Error(
    `Refusing to package: no Google OAuth client ID is baked into ${path.basename(BUNDLE)}.\n\n` +
    `Every screen in Vison is behind sign-in, so this installer would show\n` +
    `"Sign-in is not configured" and stop there for every user who installs it.\n\n` +
    `Create an OAuth 2.0 Client ID of type "Desktop app" in Google Cloud Console\n` +
    `(APIs & Services -> Credentials), then build with it set:\n\n` +
    `  $env:VISON_GOOGLE_CLIENT_ID = "<id>.apps.googleusercontent.com"\n` +
    `  npm run build\n\n` +
    `It is a public identifier, not a secret - desktop OAuth clients have no\n` +
    `client secret. To build a deliberately unusable installer for local\n` +
    `testing, set ${OVERRIDE}=1.\n` +
    `See docs/BUILD.md.`);
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
