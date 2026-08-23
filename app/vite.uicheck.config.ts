// Renderer-only dev config, used to look at the UI in a plain browser.
//
// The real config runs vite-plugin-electron, which launches Electron, which
// spawns its own backend with a random bearer token - so a browser pointed at
// that dev server can never reach the API. Dropping the plugin leaves the
// renderer alone, where window.vison is undefined, the sign-in gate is skipped
// and fetch talks to a token-free backend directly.
//
// Not part of any build. Delete freely.
import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'

export default defineConfig({
  plugins: [react()],
  server: { port: 5199, strictPort: true },
})
