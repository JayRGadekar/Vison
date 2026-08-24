import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'
import electron from 'vite-plugin-electron/simple'

// https://vite.dev/config/
export default defineConfig({
  // Bake the OAuth client ID and secret into the build.
  //
  // auth.ts reads process.env at runtime, which is fine in development but
  // useless once the app is installed - nobody sets a system environment
  // variable to launch a desktop app. Set VISON_GOOGLE_CLIENT_ID and
  // VISON_GOOGLE_CLIENT_SECRET when running the build and they travel with the
  // bundle; runtime env vars still win, so development keeps working without
  // rebuilding.
  //
  // Both are public identifiers despite the name on the second one. Google
  // requires a client_secret at the token endpoint even for "Desktop app"
  // clients, so it has to ship inside the installer, where anyone can read it
  // out of the asar. PKCE is what actually secures this flow. See
  // getClientSecret() in electron/auth.ts.
  define: {
    __VISON_GOOGLE_CLIENT_ID__: JSON.stringify(process.env.VISON_GOOGLE_CLIENT_ID || ''),
    __VISON_GOOGLE_CLIENT_SECRET__: JSON.stringify(process.env.VISON_GOOGLE_CLIENT_SECRET || ''),
  },
  plugins: [
    react(),
    electron({
      main: {
        entry: 'electron/main.ts',
        // vite-plugin-electron runs a separate build for the main process with
        // its own config, so the top-level define above does not reach it.
        vite: {
          define: {
            __VISON_GOOGLE_CLIENT_ID__: JSON.stringify(process.env.VISON_GOOGLE_CLIENT_ID || ''),
            __VISON_GOOGLE_CLIENT_SECRET__: JSON.stringify(process.env.VISON_GOOGLE_CLIENT_SECRET || ''),
          },
        },
      },
      preload: {
        input: 'electron/preload.ts',
        vite: {
          build: {
            rollupOptions: {
              output: {
                format: 'cjs',
              },
            },
          },
        },
      },
    }),
  ],
})
