// Checks what sign-in this build will actually be able to do, and refuses only
// the one combination that is worse than useless.
//
// Sign-in is optional in Vison: the app generates and stores locally, and
// nothing about that needs a Google identity. So a build with no OAuth
// credentials is a perfectly good installer - it simply offers no sign-in.
// This used to throw in that case, back when every screen sat behind the gate.
//
// What is still worth refusing is a HALF-configured build. Google requires a
// client_secret at the token endpoint even for "Desktop app" clients, so an
// installer carrying the ID alone walks the user through the account picker
// and the consent screen and only then dies with "client_secret is missing".
// Failing after consent is a worse experience than having no sign-in at all,
// and unlike the other cases it is certainly a mistake rather than a choice.
//
// It checks the shipped artifact rather than the environment, and that detail
// is load-bearing: the values reach the bundle through vite's `define`, and
// vite-plugin-electron runs a separate build for the main process. They have
// been set in the environment, reached the renderer, and silently missed the
// main-process bundle - which shipped. Reading process.env here would have
// passed that day.

const fs = require('fs');
const path = require('path');

const BUNDLE = path.resolve(__dirname, '..', 'dist-electron', 'main.js');

// Google desktop OAuth client IDs are always <digits>-<token>.apps.googleusercontent.com.
const CLIENT_ID = /[0-9]+-[a-z0-9]+\.apps\.googleusercontent\.com/;

// ...and current client secrets are GOCSPX-<token>.
const CLIENT_SECRET = /GOCSPX-[A-Za-z0-9_-]{10,}/;

const OVERRIDE = 'VISON_ALLOW_UNCONFIGURED_AUTH';

function checkAuthConfigured({ quiet = false } = {}) {
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

  if (!id && !secret) {
    if (!quiet) {
      console.warn(
        `[auth] No Google OAuth credentials baked in - this build ships without sign-in.\n` +
        `       That is a supported configuration: Vison runs fully without an account.\n` +
        `       To offer sign-in, build with VISON_GOOGLE_CLIENT_ID and\n` +
        `       VISON_GOOGLE_CLIENT_SECRET set. See docs/OAUTH-SETUP.md.`);
    }
    return;
  }

  if (process.env[OVERRIDE] === '1') {
    console.warn(
      `[auth] ${OVERRIDE}=1 - packaging a half-configured build anyway.\n` +
      `       Sign-in will fail after the user has already granted consent.`);
    return;
  }

  const present = id ? 'client ID' : 'client secret';
  const missing = id ? 'client secret' : 'client ID';

  throw new Error(
    `Refusing to package: this build has a Google OAuth ${present} but no ${missing}.\n\n` +
    `Google requires a client_secret at the token endpoint even for "Desktop app"\n` +
    `clients, so a build with only one of the two takes the user through the\n` +
    `account picker and the consent screen and then fails the exchange. Shipping\n` +
    `no sign-in at all is better than that, and is fully supported - build with\n` +
    `neither value set and the app simply runs without an account.\n\n` +
    `To offer sign-in, set both:\n\n` +
    `  $env:VISON_GOOGLE_CLIENT_ID     = "<id>.apps.googleusercontent.com"\n` +
    `  $env:VISON_GOOGLE_CLIENT_SECRET = "GOCSPX-<...>"\n` +
    `  npm run build\n\n` +
    `Both are public identifiers despite the name on the second: they ship inside\n` +
    `the installer where anyone can read them, and PKCE is what secures the flow.\n` +
    `To package this half-configured build anyway, set ${OVERRIDE}=1.\n` +
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
