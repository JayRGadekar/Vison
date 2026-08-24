import { app, BrowserWindow, globalShortcut, Tray, Menu, nativeImage, ipcMain } from 'electron';
import path from 'path';
import { fileURLToPath } from 'url';
import { spawn, ChildProcess } from 'child_process';
import os from 'os';
import http from 'http';
import fs from 'fs/promises';
import crypto from 'crypto';
import { signIn, signOut, currentUser, getClientId, getClientSecret } from './auth';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

let mainWindow: BrowserWindow | null;
let backendProcess: ChildProcess | null = null;
let tray: Tray | null = null;
let isQuitting = false;
const BACKEND_URL = 'http://127.0.0.1:11439';

// Minted fresh each launch and handed to the backend via the environment. The
// renderer sends it on every call, so the local API cannot be driven by
// anything that did not get it from us.
//
// Honest caveat: someone can still start vison_server.exe themselves with no
// token set and it will accept everything. A local app cannot enforce this,
// only make bypassing it a deliberate act.
const API_TOKEN = crypto.randomBytes(32).toString('hex');

// Suppress benign disk cache errors in development
app.commandLine.appendSwitch('disable-disk-cache');
app.commandLine.appendSwitch('disable-gpu-shader-disk-cache');
app.commandLine.appendSwitch('disable-gpu-cache'); // extra safety

// Ensure GPUCache is cleared before app starts to avoid stale lock files
const gpuCachePath = path.join(app.getPath('userData'), 'GPUCache');
fs.rm(gpuCachePath, { recursive: true, force: true }).catch(() => {});

async function getChatDir() {
  const dir = path.join(app.getPath('userData'), 'chats');
  await fs.mkdir(dir, { recursive: true });
  return dir;
}

// Chat ids arrive over the IPC bridge from the renderer, so they must never be
// pasted into a path unchecked - an id like "../../config" would let a save or
// a delete escape the chats directory. Ids we mint look like "chat_1699999999",
// so anything outside that alphabet is rejected rather than sanitised.
function chatFileName(id: unknown): string | null {
  if (typeof id !== 'string' || !/^[A-Za-z0-9_-]{1,128}$/.test(id)) return null;
  return `${id}.json`;
}

// Earlier builds wrote every chat to a file literally named ".json" (the id was
// missing from the template string), so each save clobbered the previous one.
// Rename that leftover to its real id the first time we see it, otherwise the
// user's most recent conversation would be listed but never load.
//
// This never deletes anything: a legacy file we cannot place - unreadable, no
// usable id, or an id that already has a real file - is left exactly where it
// is. chat:list skips it either way, so leaving it costs one small read per
// listing, which is a better trade than throwing away conversation history we
// merely failed to understand.
async function migrateLegacyChatFile(dir: string) {
  const legacy = path.join(dir, '.json');
  try {
    const chat = JSON.parse(await fs.readFile(legacy, 'utf-8'));
    const name = chatFileName(chat?.id);
    if (!name) return;

    const target = path.join(dir, name);
    try {
      await fs.access(target);
      return;                    // a real file already holds this id; leave the leftover alone
    } catch {
      await fs.rename(legacy, target);
    }
  } catch {
    // No legacy file, or it is unreadable - nothing to migrate.
  }
}

ipcMain.handle('chat:save', async (_event, chat: any) => {
  try {
    const name = chatFileName(chat?.id);
    if (!name) return { success: false, error: `Invalid chat id: ${String(chat?.id)}` };
    const dir = await getChatDir();
    await fs.writeFile(path.join(dir, name), JSON.stringify(chat, null, 2));
    return { success: true };
  } catch (err) {
    return { success: false, error: String(err) };
  }
});

ipcMain.handle('chat:load', async (_event, id: string) => {
  try {
    const name = chatFileName(id);
    if (!name) return { success: false, error: `Invalid chat id: ${String(id)}` };
    const dir = await getChatDir();
    await migrateLegacyChatFile(dir);
    const data = await fs.readFile(path.join(dir, name), 'utf-8');
    return { success: true, chat: JSON.parse(data) };
  } catch (err) {
    return { success: false, error: String(err) };
  }
});

// Third-party notices. Reproducing the licence text of the components Vison
// ships is an obligation of every permissive licence involved, and one that is
// only met if a user can actually reach it - so the text is read and shown in
// the app rather than left as a file in the install directory.
ipcMain.handle('app:notices', async () => {
  // Packaged: resources/licenses. Development: the generated copy in the repo.
  const candidates = app.isPackaged
    ? [path.join(process.resourcesPath, 'licenses', 'THIRD-PARTY-NOTICES.txt')]
    : [
        path.join(__dirname, '..', 'build-resources', 'licenses', 'THIRD-PARTY-NOTICES.txt'),
        path.join(__dirname, '..', '..', 'app', 'build-resources', 'licenses', 'THIRD-PARTY-NOTICES.txt'),
      ];

  for (const file of candidates) {
    try {
      return { success: true, text: await fs.readFile(file, 'utf-8') };
    } catch { /* try the next location */ }
  }
  return {
    success: false,
    error: 'Notices file not found. Run `npm run licenses` to generate it.',
  };
});

ipcMain.handle('chat:list', async () => {
  try {
    const dir = await getChatDir();
    await migrateLegacyChatFile(dir);
    const files = await fs.readdir(dir);
    const chats = [];
    for (const file of files) {
      if (file.endsWith('.json')) {
        try {
          const data = await fs.readFile(path.join(dir, file), 'utf-8');
          const chat = JSON.parse(data);
          // A chat with no usable id can never be loaded or deleted again, so
          // leave it out of the list rather than showing a dead entry.
          if (!chatFileName(chat?.id)) continue;
          chats.push({ id: chat.id, title: chat.title, timestamp: chat.timestamp });
        } catch (e) {}
      }
    }
    return { success: true, chats: chats.sort((a: any, b: any) => b.timestamp - a.timestamp) };
  } catch (err) {
    return { success: false, error: String(err) };
  }
});

ipcMain.handle('chat:delete', async (_event, id: string) => {
  try {
    const name = chatFileName(id);
    if (!name) return { success: false, error: `Invalid chat id: ${String(id)}` };
    const dir = await getChatDir();
    await fs.unlink(path.join(dir, name));
    return { success: true };
  } catch (err) {
    return { success: false, error: String(err) };
  }
});

ipcMain.handle('auth:signIn', async () => {
  try {
    return { success: true, user: await signIn() };
  } catch (err: any) {
    return { success: false, error: err?.message ?? String(err) };
  }
});

ipcMain.handle('auth:signOut', async () => {
  await signOut();
  return { success: true };
});

ipcMain.handle('auth:status', async () => {
  const user = await currentUser();
  return { signedIn: !!user, user, configured: !!getClientId() && !!getClientSecret() };
});

ipcMain.handle('backend:request', async (_event, req: { path: string; init?: { method?: string; headers?: Record<string, string>; body?: string } }) => {
  // Everything sits behind sign-in: the bridge refuses to reach the backend at
  // all without a valid session, so nothing in the renderer can get to
  // generation without one.
  const signedInUser = await currentUser();
  if (!signedInUser) {
    return { ok: false, status: 401, text: '', json: null, error: 'Not signed in' };
  }

  return new Promise((resolve) => {
    try {
      const options = {
        hostname: '127.0.0.1',        port: 11439,
        path: req.path,
        method: req.init?.method ?? 'GET',
        headers: { ...(req.init?.headers ?? {}), Authorization: `Bearer ${API_TOKEN}` },
        timeout: 30 * 60 * 1000 // 30 minutes
      };

      const request = http.request(options, (res) => {
        let data = '';
        res.on('data', (chunk) => { data += chunk; });
        res.on('end', () => {
          let json: any = null;
          try { json = data ? JSON.parse(data) : null; } catch {}
          resolve({
            ok: res.statusCode ? res.statusCode >= 200 && res.statusCode < 300 : false,
            status: res.statusCode ?? 0,
            text: data,
            json,
            error: null,
          });
        });
      });

      request.on('error', (err: any) => {
        const msg = err?.message ?? String(err);
        const cause = err?.cause ? ` [cause: ${err.cause?.message ?? String(err.cause)}]` : '';
        console.error(`Backend request to ${req.path} failed: ${msg}${cause}`);
        resolve({ ok: false, status: 0, text: '', json: null, error: `${msg}${cause}` });
      });

      request.on('timeout', () => {
        request.destroy(new Error('Request timed out after 30 minutes'));
      });

      if (req.init?.body) request.write(req.init.body);
      request.end();
    } catch (err: any) {
      resolve({ ok: false, status: 0, text: '', json: null, error: err?.message ?? String(err) });
    }
  });
});

async function isBackendReachable(timeoutMs = 1500): Promise<boolean> {
  const ctrl = new AbortController();
  const timer = setTimeout(() => ctrl.abort(), timeoutMs);
  try {
    const res = await fetch(`${BACKEND_URL}/api/models`, {
      signal: ctrl.signal,
      headers: { Authorization: `Bearer ${API_TOKEN}` },
    });
    return res.ok;
  } catch {
    return false;
  } finally {
    clearTimeout(timer);
  }
}

function getBackendExecutable() {
  const isWindows = process.platform === 'win32';

  if (app.isPackaged) {
    // extraResources copies build/bin (the CMake runtime output) to
    // resources/backend, so the exe sits next to the ggml*/visioncpp DLLs it
    // resolves at load time. The old name here was 'vison-backend-win.exe',
    // left over from the PyInstaller backend that no longer exists - nothing
    // by that name has ever been produced, so a packaged app shipped with no
    // usable backend at all.
    return path.join(process.resourcesPath, 'backend',
                     isWindows ? 'vison_server.exe' : 'vison_server');
  }

  // Development runs the freshly compiled binary straight out of the build tree.
  return path.join(__dirname, '../..', 'build', 'bin',
                   isWindows ? 'vison_server.exe' : 'vison_server');
}

// Models and generated files must land somewhere the user can actually write.
// An installed app lives in Program Files, which is read-only for a normal
// account, so the backend is told to keep its data under the per-user app
// directory instead. In development we keep using the repo so the models
// already downloaded there are found.
function getBackendDataDir() {
  return app.isPackaged ? app.getPath('userData') : path.join(__dirname, '../..');
}

// The backend can die on its own - a VRAM over-commit inside ggml aborts the
// process outright, with no exception for the C++ side to catch. Without this
// the window just sits on "Backend Offline" until the user restarts the app,
// so bring it back automatically. Bounded, because a backend that crashes on
// startup would otherwise respawn forever.
const BACKEND_MAX_RESTARTS = 3;
const BACKEND_RESTART_WINDOW_MS = 60_000;
let backendRestarts: number[] = [];
let backendStopping = false;

function handleBackendExit(code: number | null, signal: string | null) {
  console.error(`Backend process exited with code=${code} signal=${signal}`);
  backendProcess = null;

  // Deliberate shutdown (app quitting, or shutdownBackend()) is not a crash.
  if (isQuitting || backendStopping) return;

  const now = Date.now();
  backendRestarts = backendRestarts.filter(t => now - t < BACKEND_RESTART_WINDOW_MS);
  if (backendRestarts.length >= BACKEND_MAX_RESTARTS) {
    console.error(
      `Backend exited ${backendRestarts.length} times in the last ` +
      `${BACKEND_RESTART_WINDOW_MS / 1000}s; not restarting again. ` +
      `Check the backend log - this usually means it cannot allocate GPU memory.`);
    return;
  }
  backendRestarts.push(now);

  // Give the OS a moment to release the port and the GPU allocations.
  setTimeout(() => {
    if (isQuitting || backendStopping) return;
    console.log(`Restarting backend (attempt ${backendRestarts.length}/${BACKEND_MAX_RESTARTS})...`);
    void startBackend();
  }, 1000);
}

async function startBackend() {
  const backendExec = getBackendExecutable();
  console.log(`Starting backend: ${backendExec}`);

  // In production, reuse an already-running backend.
  // In development, kill it so we always use the latest compiled binary.
  if (await isBackendReachable()) {
    if (app.isPackaged) {
      console.log('Backend already running on 127.0.0.1:11439, reusing existing process.');
      return;
    } else {
      console.log('Stale dev backend detected. Shutting it down...');
      try {
        await fetch(`${BACKEND_URL}/api/shutdown`, { method: 'POST' });
        // Wait briefly for it to exit
        await new Promise(r => setTimeout(r, 1000));
      } catch (e) {
        // Fallback: kill any vison_server.exe by name if on Windows
        if (process.platform === 'win32') {
           spawn('taskkill', ['/IM', 'vison_server.exe', '/F'], { detached: true });
           await new Promise(r => setTimeout(r, 1000));
        }
      }
    }
  }

  backendStopping = false;

  const backendEnv = {
    ...process.env,
    VISON_DATA_DIR: getBackendDataDir(),
    VISON_API_TOKEN: API_TOKEN,
  };

  try {
    if (app.isPackaged) {
       backendProcess = spawn(backendExec, [], {
         detached: false,
         cwd: path.dirname(backendExec),
         env: backendEnv,
       });
    } else {
       // Dev mode: run the newly compiled C++ server
       // All runtime artifacts (exe + ggml/visioncpp DLLs) are emitted into
       // build/bin so the server can resolve its DLLs; see the root CMakeLists.
       const serverExec = process.platform === 'win32'
         ? path.join(__dirname, '../../build/bin/vison_server.exe')
         : path.join(__dirname, '../../build/bin/vison_server');
       
       console.log(`Starting dev backend: ${serverExec}`);
       backendProcess = spawn(serverExec, [], {
         cwd: path.join(__dirname, '../..'),
         detached: false,
         env: backendEnv,
       });
       
       backendProcess.stdout?.on('data', (data) => console.log(`Backend STDOUT: ${data}`));
       backendProcess.stderr?.on('data', (data) => console.error(`Backend STDERR: ${data}`));
       
       backendProcess.on('error', (err: any) => {
         console.error('Failed to start backend process:', err);
       });
       
       backendProcess.on('exit', handleBackendExit);
       return;
    }

    backendProcess.stdout?.on('data', (data) => console.log(`Backend STDOUT: ${data}`));
    backendProcess.stderr?.on('data', (data) => console.error(`Backend STDERR: ${data}`));
    backendProcess.on('exit', handleBackendExit);
  } catch (err) {
    console.error('Failed to start backend process:', err);
  }
}

async function shutdownBackend() {
  backendStopping = true;
  if (backendProcess) {
    try {
      // Graceful shutdown via API
      await fetch('http://127.0.0.1:11439/api/shutdown', { method: 'POST' });
    } catch (e) {
      console.log('Backend may have already been closed');
    }
    // Force kill if necessary
    backendProcess.kill('SIGINT');
  }
}

function createWindow() {
  mainWindow = new BrowserWindow({
    width: 900,
    height: 700,
    titleBarStyle: 'hidden', // Give it a native clean look
    titleBarOverlay: {
      color: '#181818',
      symbolColor: '#ffffff'
    },
    webPreferences: {
      preload: path.join(__dirname, 'preload.mjs'),
    },
  });

  if (process.env.VITE_DEV_SERVER_URL) {
    mainWindow.loadURL(process.env.VITE_DEV_SERVER_URL);
  } else {
    mainWindow.loadFile(path.join(__dirname, '../dist/index.html'));
  }

  mainWindow.on('close', (event) => {
    if (!isQuitting) {
      event.preventDefault();
      mainWindow?.hide();
    }
  });
}

app.whenReady().then(async () => {
  await startBackend();
  createWindow();

  // Setup System Tray
  try {
    const iconPath = app.isPackaged 
      ? path.join(process.resourcesPath, 'assets', 'icon.png')
      : path.join(__dirname, '../../assets/icon.png'); // Best effort fallback
      
    // The source icon is 1024px so it can also serve as the installer/app
    // icon; handing that straight to Tray gives Windows a badly downscaled
    // blob, so resize to the tray's actual size first.
    const rawIcon = nativeImage.createFromPath(iconPath);
    const trayIcon = rawIcon.isEmpty()
      ? nativeImage.createEmpty()
      : rawIcon.resize({ width: 16, height: 16, quality: 'best' });
    tray = new Tray(trayIcon);
    tray.setToolTip('Vison');
    
    const contextMenu = Menu.buildFromTemplate([
      { label: 'Show Vison', click: () => { mainWindow?.show(); mainWindow?.focus(); } },
      { type: 'separator' },
      { label: 'Quit', click: () => { isQuitting = true; app.quit(); } }
    ]);
    tray.setContextMenu(contextMenu);

    tray.on('click', () => {
      if (mainWindow) {
        if (mainWindow.isVisible()) {
          mainWindow.hide();
        } else {
          mainWindow.show();
          mainWindow.focus();
        }
      }
    });
  } catch (e) {
    console.error("Failed to initialize tray", e);
  }

  // Register Global Hotkey to minimize/restore
  globalShortcut.register('CommandOrControl+Alt+Space', () => {
    if (mainWindow) {
      if (mainWindow.isVisible()) {
        if (mainWindow.isFocused()) {
          mainWindow.hide(); // Hide if active
        } else {
          mainWindow.focus(); // Bring to front if visible but behind other apps
        }
      } else {
        mainWindow.show(); // Show if hidden
        mainWindow.focus();
      }
    }
  });
});

// Ensures we ask the Python server to nicely close its ports
app.on('will-quit', async (e) => {
  // Unregister all shortcuts.
  globalShortcut.unregisterAll();
  e.preventDefault();
  await shutdownBackend();
  isQuitting = true;
  process.exit(0);
});

app.on('before-quit', () => {
  isQuitting = true;
});

// Subscribing at all is what keeps Vison alive in the tray: Electron only
// quits on the last window closing when nothing handles this event. There is
// nothing to preventDefault() here - the handler's existence is the behaviour.
app.on('window-all-closed', () => {});

app.on('activate', () => {
  if (BrowserWindow.getAllWindows().length === 0) {
    createWindow();
  }
});
