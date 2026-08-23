import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'
import electron from 'vite-plugin-electron/simple'

// https://vite.dev/config/
export default defineConfig({
  // Bake the OAuth client ID into the build.
  //
  // auth.ts reads process.env at runtime, which is fine in development but
  // useless once the app is installed - nobody sets a system environment
  // variable to launch a desktop app. Set VISON_GOOGLE_CLIENT_ID when running
  // the build and it travels with the bundle; a runtime env var still wins, so
  // development keeps working without rebuilding. It is a public identifier,
  // not a secret - a desktop OAuth client has no secret to leak.
  define: {
    __VISON_GOOGLE_CLIENT_ID__: JSON.stringify(process.env.VISON_GOOGLE_CLIENT_ID || ''),
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
