import { safeStorage, shell, app } from 'electron';
import crypto from 'crypto';
import http from 'http';
import fs from 'fs/promises';
import path from 'path';
import { AddressInfo } from 'net';

// Google sign-in for a desktop app.
//
// Desktop apps cannot keep a client secret - anything shipped in the bundle is
// readable - so this uses Authorization Code with PKCE and a loopback redirect,
// which is what RFC 8252 prescribes for native apps. The verifier never leaves
// this process, and the code is worthless without it.
//
// Read this before relying on it as a paywall: Vison runs entirely on the
// user's machine. The backend listens on 127.0.0.1, the models sit on their
// disk, and the renderer ships as readable JavaScript inside the asar. Sign-in
// buys identity and friction, NOT enforcement - anyone willing to call the
// local API directly can skip it. Real enforcement needs a remote service that
// does something the local app cannot do by itself.

const GOOGLE_AUTH_URL = 'https://accounts.google.com/o/oauth2/v2/auth';
const GOOGLE_TOKEN_URL = 'https://oauth2.googleapis.com/token';
const GOOGLE_USERINFO_URL = 'https://openidconnect.googleapis.com/v1/userinfo';
const SCOPES = 'openid email profile';

export interface AuthUser {
  email: string;
  name?: string;
  picture?: string;
}

interface StoredTokens {
  refresh_token?: string;
  access_token?: string;
  expires_at?: number;   // epoch ms
  user?: AuthUser;
}

let cached: StoredTokens | null = null;

// Baked in at build time by vite.config.ts. Declared, not imported, because it
// is a compile-time substitution rather than a real module.
declare const __VISON_GOOGLE_CLIENT_ID__: string;
declare const __VISON_GOOGLE_CLIENT_SECRET__: string;

// The client ID is deployment configuration, not a secret, but it is also not
// something that should be invented here - an app with the wrong one fails at
// the consent screen with an opaque error.
//
// A runtime environment variable wins over the baked-in value so a developer
// can point a build at a different OAuth client without rebuilding.
export function getClientId(): string | null {
  const fromEnv = process.env.VISON_GOOGLE_CLIENT_ID?.trim();
  if (fromEnv) return fromEnv;
  const baked = typeof __VISON_GOOGLE_CLIENT_ID__ === 'string'
    ? __VISON_GOOGLE_CLIENT_ID__.trim() : '';
  return baked || null;
}

// Google requires a client_secret at the token endpoint even for "Desktop app"
// clients, which is a departure from RFC 8252 and from every other provider's
// native-app flow. Without it the browser half of sign-in completes normally
// and the exchange then fails with:
//
//   400 {"error":"invalid_request","error_description":"client_secret is missing."}
//
// So this is deployment configuration exactly like the client ID, and it gets
// the same treatment.
//
// It is NOT a secret in any meaningful sense, whatever Google calls it. It is
// shipped inside the installer and can be read out of the asar by anyone who
// cares to look; Google's own guidance for installed apps acknowledges this.
// PKCE is what actually protects the flow - the code_verifier never leaves this
// process, so an intercepted authorization code is useless without it. Do not
// let the name mislead you into thinking a leaked value here is a breach; do
// not reuse this value anywhere that a real secret is expected either.
export function getClientSecret(): string | null {
  const fromEnv = process.env.VISON_GOOGLE_CLIENT_SECRET?.trim();
  if (fromEnv) return fromEnv;
  const baked = typeof __VISON_GOOGLE_CLIENT_SECRET__ === 'string'
    ? __VISON_GOOGLE_CLIENT_SECRET__.trim() : '';
  return baked || null;
}

function tokenFile() {
  return path.join(app.getPath('userData'), 'auth.json');
}

// safeStorage encrypts against the OS keychain/DPAPI, so the refresh token is
// not sitting in plain text in a file any process can read. It is unavailable
// on some Linux setups, where we degrade to plaintext rather than refusing to
// work - and say so.
async function persist(tokens: StoredTokens) {
  cached = tokens;
  const json = JSON.stringify(tokens);
  try {
    if (safeStorage.isEncryptionAvailable()) {
      const blob = safeStorage.encryptString(json);
      await fs.writeFile(tokenFile(), blob);
      return;
    }
  } catch (err) {
    console.error('safeStorage failed, falling back to plaintext:', err);
  }
  console.warn('Storing auth tokens unencrypted: OS keychain unavailable.');
  await fs.writeFile(tokenFile(), json, 'utf-8');
}

async function load(): Promise<StoredTokens | null> {
  if (cached) return cached;
  try {
    const raw = await fs.readFile(tokenFile());
    let json: string;
    try {
      json = safeStorage.isEncryptionAvailable()
        ? safeStorage.decryptString(raw)
        : raw.toString('utf-8');
    } catch {
      json = raw.toString('utf-8');   // written before encryption was available
    }
    cached = JSON.parse(json);
    return cached;
  } catch {
    return null;
  }
}

export async function signOut() {
  cached = null;
  try { await fs.unlink(tokenFile()); } catch { /* already gone */ }
}

function base64url(buf: Buffer) {
  return buf.toString('base64').replace(/\+/g, '-').replace(/\//g, '_').replace(/=/g, '');
}

// Starts the loopback listener and reports the port it actually got, so the
// redirect URI can be built to match. The port has to be known BEFORE the
// browser is opened - probing with a second throwaway server would hand back a
// port this listener is not on.
function startRedirectListener(expectedState: string): Promise<{
  port: number;
  code: Promise<string>;
}> {
  return new Promise((resolveStart, rejectStart) => {
    let resolveCode!: (code: string) => void;
    let rejectCode!: (err: Error) => void;
    const code = new Promise<string>((res, rej) => { resolveCode = res; rejectCode = rej; });

    const server = http.createServer((req, res) => {
      const url = new URL(req.url ?? '/', `http://127.0.0.1`);
      const code = url.searchParams.get('code');
      const state = url.searchParams.get('state');
      const error = url.searchParams.get('error');

      const reply = (message: string) => {
        res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
        res.end(`<!doctype html><meta charset="utf-8">
          <body style="font-family:system-ui;background:#181818;color:#eee;
                       display:flex;align-items:center;justify-content:center;height:100vh">
            <p>${message}</p></body>`);
      };

      if (error) {
        reply('Sign-in was cancelled. You can close this tab.');
        server.close();
        rejectCode(new Error(`Google returned: ${error}`));
        return;
      }
      // A mismatched state means this redirect did not come from the request we
      // started, so the code must not be trusted.
      if (!code || state !== expectedState) {
        reply('Sign-in failed. You can close this tab.');
        server.close();
        rejectCode(new Error('Missing code, or state did not match the request'));
        return;
      }

      reply('Signed in to Vison. You can close this tab.');
      server.close();
      resolveCode(code);
    });

    server.on('error', rejectStart);
    server.listen(0, '127.0.0.1', () => {
      const port = (server.address() as AddressInfo).port;
      resolveStart({ port, code });
    });

    setTimeout(() => {
      server.close();
      rejectCode(new Error('Timed out waiting for sign-in'));
    }, 5 * 60 * 1000);
  });
}

async function postForm(url: string, form: Record<string, string>) {
  const res = await fetch(url, {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: new URLSearchParams(form).toString(),
  });
  const body = await res.json().catch(() => ({}));
  if (!res.ok) {
    throw new Error(`${url} returned ${res.status}: ${JSON.stringify(body)}`);
  }
  return body as any;
}

export async function signIn(): Promise<AuthUser> {
  const clientId = getClientId();
  if (!clientId) {
    throw new Error(
      'No Google client ID configured. Set VISON_GOOGLE_CLIENT_ID to the OAuth ' +
      '"Desktop app" client ID from Google Cloud Console.');
  }

  // Checked before the browser opens, not after. Google rejects the exchange at
  // the very end of the flow, so without this the user picks an account, grants
  // consent, and only then sees a failure they can do nothing about.
  const clientSecret = getClientSecret();
  if (!clientSecret) {
    throw new Error(
      'No Google client secret configured. Set VISON_GOOGLE_CLIENT_SECRET to ' +
      'the secret shown beside the client ID in Google Cloud Console. Google ' +
      'requires it even for "Desktop app" clients.');
  }

  const verifier = base64url(crypto.randomBytes(32));
  const challenge = base64url(crypto.createHash('sha256').update(verifier).digest());
  const state = base64url(crypto.randomBytes(16));

  // Listen first, then build the redirect URI from the port we actually got.
  // Opening the browser before the listener is up races the redirect against a
  // closed port.
  const { port, code: pendingCode } = await startRedirectListener(state);
  const redirectUri = `http://127.0.0.1:${port}`;
  const authUrl = `${GOOGLE_AUTH_URL}?` + new URLSearchParams({
    client_id: clientId,
    redirect_uri: redirectUri,
    response_type: 'code',
    scope: SCOPES,
    code_challenge: challenge,
    code_challenge_method: 'S256',
    state,
    access_type: 'offline',     // ask for a refresh token
    prompt: 'consent',
  }).toString();

  // The system browser, not a BrowserWindow: Google blocks OAuth in embedded
  // webviews, and it lets the user reuse an existing session.
  await shell.openExternal(authUrl);

  const code = await pendingCode;

  const tokens = await postForm(GOOGLE_TOKEN_URL, {
    client_id: clientId,
    client_secret: clientSecret,
    code,
    code_verifier: verifier,
    grant_type: 'authorization_code',
    redirect_uri: redirectUri,
  });

  const userRes = await fetch(GOOGLE_USERINFO_URL, {
    headers: { Authorization: `Bearer ${tokens.access_token}` },
  });
  const profile = await userRes.json() as any;

  const user: AuthUser = {
    email: profile.email,
    name: profile.name,
    picture: profile.picture,
  };

  await persist({
    refresh_token: tokens.refresh_token,
    access_token: tokens.access_token,
    expires_at: Date.now() + (tokens.expires_in ?? 3600) * 1000,
    user,
  });

  return user;
}

// Returns the signed-in user, refreshing the access token when it has expired.
// null means "not signed in" rather than an error, so callers can just gate.
export async function currentUser(): Promise<AuthUser | null> {
  const tokens = await load();
  if (!tokens?.user) return null;

  const stillValid = tokens.expires_at && tokens.expires_at > Date.now() + 60_000;
  if (stillValid) return tokens.user;

  if (!tokens.refresh_token) return null;

  const clientId = getClientId();
  if (!clientId) return null;

  // The refresh grant is authenticated the same way the code exchange is, so a
  // build with the ID but no secret would sign in once and then fail to resume
  // the session an hour later - a much more confusing failure than not signing
  // in at all.
  const clientSecret = getClientSecret();
  if (!clientSecret) return null;

  try {
    const refreshed = await postForm(GOOGLE_TOKEN_URL, {
      client_id: clientId,
      client_secret: clientSecret,
      refresh_token: tokens.refresh_token,
      grant_type: 'refresh_token',
    });
    await persist({
      ...tokens,
      access_token: refreshed.access_token,
      expires_at: Date.now() + (refreshed.expires_in ?? 3600) * 1000,
    });
    return tokens.user;
  } catch (err) {
    // A revoked or expired refresh token means the session is genuinely over.
    console.error('Token refresh failed, signing out:', err);
    await signOut();
    return null;
  }
}
