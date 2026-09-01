# Security

## Reporting

Use GitHub's private reporting: **Security → Report a vulnerability** on this
repository. That opens a channel only you and I can see. Please do not open a
public issue for something exploitable.

This is a one-person project, so there is no response-time guarantee. I will
acknowledge what I can, and I would rather hear about something late than not
at all.

## What is and is not a vulnerability here

Vison runs entirely on the machine it is installed on, so the interesting
boundary is a local one. A few things look alarming and are not, and it saves
everyone time to say so up front.

**Not vulnerabilities:**

- **The Google OAuth client ID and client secret are readable in the
  installer.** They are compiled into the app and can be pulled out of the
  asar by anyone. This is how Google's desktop flow works — it requires a
  `client_secret` at the token endpoint even for "Desktop app" clients, which
  means the value cannot be kept anywhere else. PKCE is what actually secures
  the exchange, and the verifier never leaves the app process. See
  [docs/OAUTH-SETUP.md](docs/OAUTH-SETUP.md).

- **`vison_server.exe` accepts requests without a token if you start it
  yourself.** The app mints a bearer token per launch and passes it to the
  backend it spawns, so nothing that did not come from the app can drive that
  instance. A user starting the binary by hand with no token set is not
  bypassing a control — it is their machine and their process.

**Worth reporting:**

- Anything that gets code or a file path out of the renderer and into the main
  process or the OS — the renderer is the untrusted side of this app.
- A way to make the app reach the network with anything other than a Hugging
  Face model download or a Google sign-in the user asked for.
- A way for a downloaded model file, a prompt, or a restored conversation to
  execute anything.
- Anything that reads or writes outside Vison's own directories.

## Scope

Model weights are downloaded from Hugging Face and are not audited by this
project. A malicious file published upstream is a real risk and not one Vison
can rule out — the mitigation is that nothing is downloaded until you ask for
it, from a registry you can read in `server/src/server.cpp`.
