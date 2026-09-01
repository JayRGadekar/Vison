import React, { useState, useEffect, useRef } from 'react';
import visonLogo from './assets/icon.png';
import { conversationTitle } from './conversation-title';
import { FUNDING_LINKS, CONTRIBUTION_LINKS } from './support-links';
import { Sparkles, Maximize2, Columns, RectangleHorizontal, RectangleVertical, Frame, Wand2, Settings, PenBox, Plus, ChevronDown, X, Library, Image as ImageIcon, Video, Maximize, ArrowUp, Download, Loader2, Trash2, CheckCircle2, Square, RefreshCcw, Wifi, History, LogOut, LogIn, User, Search, Heart, ExternalLink } from 'lucide-react';

interface AuthUser {
  email: string;
  name?: string;
  picture?: string;
}

declare global {
  interface Window {
    vison?: {
      request: (
        path: string,
        init?: { method?: string; headers?: Record<string, string>; body?: string }
      ) => Promise<{ ok: boolean; status: number; text: string; json: any; error: string | null }>;
      notices?: () => Promise<{ success: boolean; text?: string; error?: string }>;
      openExternal?: (url: string) => Promise<{ success: boolean; error?: string }>;
      auth?: {
        signIn: () => Promise<{ success: boolean; user?: AuthUser; error?: string }>;
        signOut: () => Promise<{ success: boolean }>;
        status: () => Promise<{ signedIn: boolean; user?: AuthUser; configured: boolean }>;
      };
      chat?: {
        save: (chat: any) => Promise<{ success: boolean; error?: string }>;
        load: (id: string) => Promise<{ success: boolean; chat?: any; error?: string }>;
        list: () => Promise<{ success: boolean; chats?: any[]; error?: string }>;
        delete: (id: string) => Promise<{ success: boolean; error?: string }>;
        search: (query: string) => Promise<{ success: boolean; results?: SearchHit[]; error?: string }>;
      };
    };
  }
}

// A conversation matching a search, with the matching text already split into
// plain and highlighted runs by the main process.
interface SearchHit {
  id: string;
  title: string;
  timestamp: number;
  parts: { text: string; match: boolean }[];
}

// What one generation cost on THIS machine.
//
// The app must not carry a speed table baked in from whatever laptop it was
// developed on - a 5090 and a 6GB laptop chip are two orders of magnitude
// apart, and a hardcoded estimate would be wrong for nearly everyone. So it
// measures instead: every completed run is recorded, and the next estimate is
// scaled off the most recent comparable one.
interface RunRecord {
  task: string;
  model: string;
  pixels: number;   // width * height
  frames: number;
  steps: number;
  seconds: number;
  at: number;
}

const NL = String.fromCharCode(10);
const RUN_HISTORY_KEY = 'vison.runHistory.v1';
const RUN_HISTORY_MAX = 40;

function loadRunHistory(): RunRecord[] {
  try {
    const raw = localStorage.getItem(RUN_HISTORY_KEY);
    const parsed = raw ? JSON.parse(raw) : [];
    return Array.isArray(parsed) ? parsed : [];
  } catch {
    return [];   // corrupt or unavailable storage just means no estimates yet
  }
}

function recordRun(rec: RunRecord) {
  try {
    const next = [rec, ...loadRunHistory()].slice(0, RUN_HISTORY_MAX);
    localStorage.setItem(RUN_HISTORY_KEY, JSON.stringify(next));
  } catch { /* estimates are a nicety; never fail a generation over them */ }
}

// Seconds this run is likely to take, or null when there is nothing to learn
// from yet. Prefers a reference from the same model, falling back to the same
// task, because a 5B transformer and a 1.3B one are not interchangeable rulers.
function estimateSeconds(
  history: RunRecord[], task: string, model: string,
  pixels: number, frames: number, steps: number,
): number | null {
  const ref = history.find(r => r.model === model && r.task === task)
           ?? history.find(r => r.task === task);
  if (!ref || !ref.seconds || !ref.pixels || !ref.frames || !ref.steps) return null;

  // Linear in each dimension. The true curve is not linear - decode grows
  // faster than pixels once tiling kicks in - but a reference measured on the
  // user's own hardware beats a better-shaped curve calibrated on someone
  // else's, and this only has to be right enough to say "minutes" or "hours".
  //
  // Checked against this session's runs, scaling from a 320x320x33 reference:
  // 480x832x33 predicted 906s against 866s actual, which is the accuracy that
  // matters. It under-predicts very short runs badly (14s against 88s) because
  // model load and per-tile decode cost do not shrink with frame count, hence
  // the floor - nobody abandons a job over the difference between 15s and 90s.
  const scaled = ref.seconds
       * (pixels / ref.pixels)
       * (frames / ref.frames)
       * (steps / ref.steps);
  return Math.max(20, scaled);
}

function formatDuration(seconds: number): string {
  if (seconds < 90) return `${Math.round(seconds)}s`;
  const m = seconds / 60;
  if (m < 90) return `${m < 10 ? m.toFixed(1) : Math.round(m)} min`;
  const h = m / 60;
  return `${h < 10 ? h.toFixed(1) : Math.round(h)} hours`;
}

// The registry declares an exact Content-Length per file plus a rounded
// headline size_gb. Summing the files is both exact and self-maintaining:
// adding a file to a model updates the badge without anyone remembering to
// bump size_gb, which is only the fallback here.
function formatBytes(bytes: number): string {
  const gb = bytes / 1024 ** 3;
  if (gb <= 0) return "-";
  return gb < 1 ? `${Math.round(gb * 1024)} MB` : `${gb.toFixed(1)} GB`;
}

function modelBytes(model: any): number {
  let bytes = 0;
  for (const f of model?.files ?? []) bytes += Number(f.size_bytes) || 0;
  return bytes > 0 ? bytes : (Number(model?.size_gb) || 0) * 1024 ** 3;
}

function formatSize(model: any): string {
  return formatBytes(modelBytes(model));
}

// Registry names carry their tier in parentheses - "Z-Image Turbo (Balanced)",
// "Wan 2.2 T2V A14B (Best Photorealism)" - which is what you want when
// comparing models in the library and only noise once one is chosen. In the
// compact pill it was the half that survived truncation ("Z-Image Turbo
// (Balan..."), hiding nothing useful and losing nothing but characters.
function shortModelName(name: string): string {
  return name.replace(/\s*\([^)]*\)\s*$/, '').trim() || name;
}

interface Compatibility {
  fit: 'good' | 'tight' | 'unsupported';
  summary: string;
  needs_ram_gb: number;
  system_ram_gb: number;
  vram_gb: number;
  tier: string;
}

// Tells the user, before they spend twenty minutes downloading, whether a model
// will actually run on their machine. The backend does the arithmetic against
// real VRAM/RAM (see check_compatibility in core/src/device.cpp); this only
// renders the verdict.
function CompatibilityNote({ compatibility }: { compatibility?: Compatibility }) {
  // Older backends, and any model the registry has no footprint for, simply
  // omit this. Showing nothing is better than showing a guess.
  if (!compatibility?.summary) return null;

  const style = {
    good:        { border: 'border-emerald-900/60', bg: 'bg-emerald-950/30', text: 'text-emerald-300', label: 'Runs well'   },
    tight:       { border: 'border-amber-900/60',   bg: 'bg-amber-950/30',   text: 'text-amber-300',   label: 'Tight fit'   },
    unsupported: { border: 'border-red-900/60',     bg: 'bg-red-950/30',     text: 'text-red-300',     label: 'Too large'   },
  }[compatibility.fit] ?? { border: 'border-[#343434]', bg: 'bg-[#181818]', text: 'text-gray-400', label: 'Unknown' };

  return (
    <div className={`mb-4 px-3 py-2 rounded-lg border ${style.border} ${style.bg}`}>
      <div className={`text-xs font-semibold mb-1 ${style.text}`}>{style.label}</div>
      <p className="text-xs text-gray-400 leading-relaxed">{compatibility.summary}</p>
    </div>
  );
}

function App() {
  // Auth gate. `null` means "still checking" - distinct from "signed out", so
  // the sign-in screen does not flash on every launch for an existing session.
  const [authUser, setAuthUser] = useState<AuthUser | null | undefined>(undefined);
  const [authConfigured, setAuthConfigured] = useState(true);
  const [authBusy, setAuthBusy] = useState(false);
  const [authError, setAuthError] = useState<string | null>(null);

  const [prompt, setPrompt] = useState("");
  const [selectedModel, setSelectedModel] = useState({ id: "stabilityai/sdxl-turbo", name: "SDXL Turbo", task: "image" });
  const [showModelDropdown, setShowModelDropdown] = useState(false);
  const [searchModel, setSearchModel] = useState("");
  const [messages, setMessages] = useState<{ role: string, content: string, url?: string, baseImage?: string }[]>([]);
  const [isGenerating, setIsGenerating] = useState(false);
  const [progress, setProgress] = useState(0);
  
  // App views
  const [currentView, setCurrentView] = useState('chat'); // 'chat' | 'library'
  const [libraryModels, setLibraryModels] = useState<{ image: any[], video: any[], image_upscale: any[], video_upscale: any[] }>({ image: [], video: [], image_upscale: [], video_upscale: [] });
  const [activeTab, setActiveTab] = useState('image'); // 'image' | 'image_upscale' | 'video' | 'video_upscale'
  const [mediaType, setMediaType] = useState<'image' | 'video'>('image');
  const [taskMode, setTaskMode] = useState<'generate' | 'upscale'>('generate');
  const [aspectRatio, setAspectRatio] = useState('16:9'); // default task for chat dropdown filter

  // Chat History State
  const [conversations, setConversations] = useState<{id: string, title: string, timestamp: number}[]>([]);
  const [currentConversationId, setCurrentConversationId] = useState<string | null>(null);
  const [showSidebar, setShowSidebar] = useState(false);
  const [showAccountMenu, setShowAccountMenu] = useState(false);
  const [chatQuery, setChatQuery] = useState('');
  const [chatResults, setChatResults] = useState<SearchHit[] | null>(null);
  const accountMenuRef = useRef<HTMLDivElement>(null);

  // Opening a conversation must not rewrite it.
  //
  // Loading sets `messages`, which is what the save effect below watches, so
  // without this every load stamped the conversation with the current time.
  // The sidebar shows that timestamp and sorts by it, so merely browsing your
  // history reordered it and destroyed the record of when anything actually
  // happened. Set just before the state updates a load causes; the effect
  // consumes it.
  const skipSaveRef = useRef(false);

  // The id has to be readable synchronously. Read from state, two message
  // updates that land in the same render batch both see null, mint two
  // different ids, and one conversation becomes two.
  const conversationIdRef = useRef<string | null>(null);

  // Attachments
  const [attachedImage, setAttachedImage] = useState<string | null>(null);
  const fileInputRef = useRef<HTMLInputElement>(null);

  // Settings State
  const [showSettings, setShowSettings] = useState(false);
  const [noticesText, setNoticesText] = useState<string | null>(null);
  const [showSupport, setShowSupport] = useState(false);
  const [supportError, setSupportError] = useState<string | null>(null);

  // Hand a support link to the main process, which opens it in the real
  // browser. It refuses anything not in src/support-links.ts, so a failure
  // here is either a missing bridge or a browser that would not launch -
  // both worth saying out loud, because the alternative is a button that
  // looks broken.
  const openSupportLink = async (url: string) => {
    setSupportError(null);
    const open = window.vison?.openExternal;
    if (!open) {
      setSupportError(`Could not open your browser. The link is ${url}`);
      return;
    }
    const result = await open(url);
    if (!result?.success) {
      setSupportError(`Could not open your browser. The link is ${url}`);
    }
  };

  const [showAdvanced, setShowAdvanced] = useState(false);
  const [runHistory, setRunHistory] = useState<RunRecord[]>(() => loadRunHistory());
  // Real GPUs reported by the backend. `index` is exactly what gpu_id expects,
  // so the picker can offer cards by name instead of asking for a magic number.
  const [gpuDevices, setGpuDevices] = useState<any[]>([]);
  // undefined until /api/system answers, so the warning never flashes before we
  // actually know whether a muxer is present.
  const [hasFfmpeg, setHasFfmpeg] = useState<boolean | undefined>(undefined);
  const [settings, setSettings] = useState({
    width: 512, height: 512, steps: 4, guidance: 1.0, seed: -1, negativePrompt: "",
    upscaleQuality: "4x", tileSize: 0, ttaMode: false, compression: 0, outputFormat: 'png', gpuId: '', allowFallback: false,
    videoDuration: 2, fps: 16
  });
  
  // Custom Downloads State
  const [downloadingModels, setDownloadingModels] = useState<{ [key: string]: boolean }>({});
  const [downloadProgressStates, setDownloadProgressStates] = useState<{ [key: string]: number }>({});
  const [localModels, setLocalModels] = useState<string[]>([]);
  // Bytes of each model already on disk. Models share files, so this is not
  // derivable from `localModels` - a model can be 0% "downloaded" and still
  // have most of its weight present because another model brought it in.
  const [presentBytes, setPresentBytes] = useState<Record<string, number>>({});
  // /api/models/local answers two questions at once; keep unpacking it in one
  // place so a caller cannot refresh one half and leave the other stale.
  const applyLocal = (data: any) => {
    setLocalModels(data?.downloaded || []);
    setPresentBytes(data?.present_bytes || {});
  };
  const [isConnected, setIsConnected] = useState<boolean | null>(null);
  const previousDownloadStatesRef = useRef<{ [key: string]: boolean }>({});
  const [diagnosticLoading, setDiagnosticLoading] = useState(false);
  const [diagnosticResult, setDiagnosticResult] = useState<string | null>(null);

    // Resolve the stored session once at startup.
    //
    // Sign-in is optional, so this only decides what the account menu shows.
    // It used to decide whether the app rendered at all, which meant anyone
    // building from source without their own OAuth client got a dead end.
    useEffect(() => {
      const api = window.vison?.auth;
      if (!api) {
        // A plain browser (vite dev) rather than Electron: no OAuth bridge, so
        // there is nothing to sign in to. The app runs regardless, and the
        // account menu simply offers no sign-in item.
        setAuthUser(null);
        setAuthConfigured(false);
        return;
      }
      api.status()
        .then(s => {
          setAuthUser(s.signedIn && s.user ? s.user : null);
          setAuthConfigured(s.configured);
        })
        .catch(() => setAuthUser(null));
    }, []);

    const handleSignIn = async () => {
      const api = window.vison?.auth;
      if (!api) return;
      setAuthBusy(true);
      setAuthError(null);
      try {
        const res = await api.signIn();
        if (res.success && res.user) setAuthUser(res.user);
        else setAuthError(res.error ?? 'Sign-in failed');
      } catch (err: any) {
        setAuthError(err?.message ?? String(err));
      } finally {
        setAuthBusy(false);
      }
    };

    const handleSignOut = async () => {
      await window.vison?.auth?.signOut();
      setAuthUser(null);
    };

    const messagesEndRef = useRef<HTMLDivElement>(null);
    const textareaRef = useRef<HTMLTextAreaElement>(null);
    const abortControllerRef = useRef<AbortController | null>(null);

    const backendRequest = async (path: string, init?: RequestInit) => {
      const toHeaderRecord = (headers?: HeadersInit): Record<string, string> => {
        const out: Record<string, string> = {};
        if (!headers) return out;
        if (headers instanceof Headers) {
          headers.forEach((value, key) => { out[key] = value; });
          return out;
        }
        if (Array.isArray(headers)) {
          headers.forEach(([k, v]) => { out[k] = String(v); });
          return out;
        }
        return headers as Record<string, string>;
      };

      if (window.vison?.request) {
        const res = await window.vison.request(path, {
          method: init?.method,
          headers: toHeaderRecord(init?.headers),
          body: typeof init?.body === 'string' ? init.body : undefined,
        });
        if (res.error) throw new Error(res.error);
        return { ok: res.ok, status: res.status, data: res.json, text: res.text };
      }

      try {
        const response = await fetch(`http://127.0.0.1:11439${path}`, init);
        const text = await response.text();
        let data: any = null;
        try {
          data = text ? JSON.parse(text) : null;
        } catch {
          data = null;
        }
        return { ok: response.ok, status: response.status, data, text };
      } catch (err: any) {
        throw new Error(`IPC bridge unavailable and direct backend fetch failed: ${String(err?.message ?? err)}`);
      }
    };

    useEffect(() => {
        messagesEndRef.current?.scrollIntoView({ behavior: 'smooth' });
    }, [messages, isGenerating, progress]);

    const handleInput = (e: React.ChangeEvent<HTMLTextAreaElement>) => {
        setPrompt(e.target.value);
        if (textareaRef.current) {
            textareaRef.current.style.height = 'auto';
            textareaRef.current.style.height = `${Math.min(textareaRef.current.scrollHeight, 200)}px`;
        }
    };

    useEffect(() => {
        if (!prompt && textareaRef.current) {
            textareaRef.current.style.height = 'auto';
        }
    }, [prompt]);

    const handleCancelGeneration = async () => {
        if (abortControllerRef.current) {
            abortControllerRef.current.abort();
        }
        try {
          await backendRequest('/api/generate/cancel', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: '{}'
          });
        } catch (err) {
          console.error('Cancel generation request failed:', err);
        }
    };
  useEffect(() => {
    // Health check loop
    const checkHealth = () => {
        backendRequest('/api/models')
          .then(({ ok }) => setIsConnected(ok))
          .catch(() => setIsConnected(false));
    };
    checkHealth();
    const interval = setInterval(checkHealth, 3000);
    return () => clearInterval(interval);
  }, []);

    useEffect(() => {
        // Capture the bridge once: TypeScript cannot keep the narrowing from the
        // outer guard alive inside these nested callbacks.
        const chatApi = window.vison?.chat;
        if (chatApi) {
          chatApi.list().then(res => {
            if (res.success && res.chats) {
              setConversations(res.chats);
              if (res.chats.length > 0 && !currentConversationId) {
                 chatApi.load(res.chats[0].id).then(r => {
                    if (r.success && r.chat) {
                       // Resuming the last conversation at launch is a load, not
                       // a change. Without the guard, starting the app was
                       // enough to restamp it.
                       skipSaveRef.current = true;
                       conversationIdRef.current = r.chat.id;
                       setCurrentConversationId(r.chat.id);
                       setMessages(r.chat.messages || []);
                    }
                 });
              }
            }
          });
        }
    }, []);

    useEffect(() => {
      // A load already wrote these messages to state; they are on disk exactly
      // as they are here, so saving would only move the clock forward.
      if (skipSaveRef.current) {
        skipSaveRef.current = false;
        return;
      }

      if (messages.length > 0 && window.vison?.chat && !isGenerating) {
        let id = conversationIdRef.current;
        if (!id) {
          id = `chat_${Date.now()}`;
          conversationIdRef.current = id;
          setCurrentConversationId(id);
        }
        const title = conversationTitle(messages);
        const timestamp = Date.now();
        window.vison.chat.save({ id, title, timestamp, messages });
        setConversations(prev => {
          const filtered = prev.filter(c => c.id !== id);
          return [{ id: id!, title, timestamp }, ...filtered].sort((a, b) => b.timestamp - a.timestamp);
        });
      }
    }, [messages, isGenerating, currentConversationId]);

    const loadConversation = async (id: string) => {
      if (window.vison?.chat) {
        const res = await window.vison.chat.load(id);
        if (res.success && res.chat) {
          skipSaveRef.current = true;
          conversationIdRef.current = id;
          setCurrentConversationId(id);
          setMessages(res.chat.messages || []);
          setCurrentView('chat');
        }
      }
    };

    const deleteConversation = async (e: React.MouseEvent, id: string) => {
      e.stopPropagation();
      if (!confirm("Delete this chat?")) return;
      if (window.vison?.chat) {
         await window.vison.chat.delete(id);
         setConversations(prev => prev.filter(c => c.id !== id));
         if (currentConversationId === id) {
            createNewChat();
         }
      }
    };

    const createNewChat = () => {
      // Clearing to an empty conversation is also not a change worth saving -
      // and the save effect ignores an empty message list anyway. Resetting the
      // ref is what makes the next message mint a fresh id rather than
      // overwriting the conversation just left.
      skipSaveRef.current = true;
      conversationIdRef.current = null;
      setCurrentConversationId(null);
      setMessages([]);
      setCurrentView('chat');
    };

    // The settings panel lives in the chat view, so reaching it from the
    // account menu has to bring the view along - otherwise the item does
    // nothing at all from the library.
    const openSettingsFromMenu = () => {
      setShowAccountMenu(false);
      setCurrentView('chat');
      setShowSettings(true);
    };

    // Search runs in the main process against the FTS index, not over the
    // conversations already in memory: the sidebar only ever holds titles, and
    // the point is to find a conversation by something said inside it.
    //
    // Debounced because every keystroke is a query. 150ms is under the point
    // where typing feels laggy and still collapses a burst of keystrokes into
    // one query.
    useEffect(() => {
      const q = chatQuery.trim();
      if (!q) { setChatResults(null); return; }

      const api = window.vison?.chat;
      if (!api?.search) { setChatResults([]); return; }

      let cancelled = false;
      const timer = setTimeout(() => {
        api.search(q).then(res => {
          // A slower earlier query must not overwrite a newer one's results.
          if (cancelled) return;
          setChatResults(res.success && res.results ? res.results : []);
        });
      }, 150);

      return () => { cancelled = true; clearTimeout(timer); };
    }, [chatQuery, conversations]);

    // A menu that only closes by clicking its own button is a trap: every other
    // menu on the platform closes on an outside click or Escape, and one that
    // does not feels broken rather than deliberate.
    useEffect(() => {
      if (!showAccountMenu) return;

      const onPointerDown = (e: MouseEvent) => {
        if (!accountMenuRef.current?.contains(e.target as Node)) setShowAccountMenu(false);
      };
      const onKeyDown = (e: KeyboardEvent) => {
        if (e.key === 'Escape') setShowAccountMenu(false);
      };

      // Capture phase, so a click on a control that stops propagation still
      // dismisses the menu rather than leaving it stranded over the new view.
      document.addEventListener('mousedown', onPointerDown, true);
      document.addEventListener('keydown', onKeyDown);
      return () => {
        document.removeEventListener('mousedown', onPointerDown, true);
        document.removeEventListener('keydown', onKeyDown);
      };
    }, [showAccountMenu]);

  useEffect(() => {
    if (isConnected) {
      backendRequest('/api/models')
        .then(({ data }) => setLibraryModels(data))
        .catch(console.error);
        
      // Fetch local models
      backendRequest('/api/models/local')
        .then(({ data }) => applyLocal(data))
        .catch(console.error);

      // Real GPU list, so the gpu_id picker can name the cards, plus whether a
      // muxer exists for video.
      backendRequest('/api/system')
        .then(({ data }) => {
          setGpuDevices(Array.isArray(data?.devices) ? data.devices : []);
          if (typeof data?.ffmpeg === 'boolean') setHasFfmpeg(data.ffmpeg);
        })
        .catch(console.error);
    }
  }, [isConnected]);

  // Live progress over WebSocket. The C++ backend serves /api/ws/progress and
  // pushes {type:"progress",step,total} during sampling plus generation_status
  // and download_status events. REST polling below is kept as a fallback for
  // download state so the UI still recovers if the socket drops.
  useEffect(() => {
    if (!isConnected) return;

    let ws: WebSocket | null = null;
    let reconnectTimer: ReturnType<typeof setTimeout> | null = null;
    let closedByUs = false;

    const connect = () => {
      try {
        ws = new WebSocket('ws://127.0.0.1:11439/api/ws/progress');
      } catch (err) {
        console.error('progress socket failed to open', err);
        return;
      }

      ws.onmessage = (event) => {
        let msg: any;
        try {
          msg = JSON.parse(event.data);
        } catch {
          return;
        }

        if (msg.type === 'progress' && typeof msg.step === 'number' && msg.total > 0) {
          setProgress(Math.round((msg.step / msg.total) * 100));
        } else if (msg.type === 'generation_status') {
          if (msg.status === 'started') setProgress(0);
          if (msg.status === 'completed') setProgress(100);
          if (msg.status === 'cancelled' || msg.status === 'error') setProgress(0);
        } else if (msg.type === 'download_progress' && msg.model) {
          setDownloadProgressStates((prev) => ({ ...prev, [msg.model]: msg.progress ?? 0 }));
        }
      };

      ws.onclose = () => {
        if (closedByUs) return;
        // The backend restarts during development; keep trying to reattach.
        reconnectTimer = setTimeout(connect, 2000);
      };

      ws.onerror = () => ws?.close();
    };

    connect();

    return () => {
      closedByUs = true;
      if (reconnectTimer) clearTimeout(reconnectTimer);
      ws?.close();
    };
  }, [isConnected]);

    useEffect(() => {
      // Poll download status from the enhanced REST endpoint.
      const syncDownloadStatus = () => {
        backendRequest('/api/download/status')
          .then(async ({ data }) => {
            if (!data) return;

            const modelId = data.model_id || '';
            const status = data.status || 'idle';
            const progress = data.progress ?? 0;
            const errorMessage = data.error_message || '';

            // Build the downloading map (for backward compatibility with UI)
            const isActive = status === 'downloading' && !!modelId;
            const newDownloadingMap: { [key: string]: boolean } = {};
            if (isActive) {
              newDownloadingMap[modelId] = true;
            }
            setDownloadingModels(newDownloadingMap);

            // Use real progress from backend instead of fake +1 increments
            setDownloadProgressStates(prev => {
              const next = { ...prev };
              if (isActive) {
                next[modelId] = progress;
              } else {
                // Download finished or errored — clean up progress
                if (modelId) delete next[modelId];
              }
              return next;
            });

            // Detect download completion (was downloading, now idle)
            const prevMap = previousDownloadStatesRef.current;
            const wasDownloading = Object.keys(prevMap).some(id => prevMap[id]);
            const nowIdle = !isActive;

            if (wasDownloading && nowIdle) {
              if (status === 'error') {
                // Download failed — notify user
                const backendError = errorMessage ? `\n\nBackend error: ${errorMessage}` : '';
                alert(`Download failed for model: ${modelId}\n\nPlease check your network connection or try again.${backendError}`);
              } else {
                // Download succeeded — refresh local models list
                try {
                  const { data: localData } = await backendRequest('/api/models/local');
                  applyLocal(localData);
                } catch {
                  // Ignore transient errors; next poll will retry.
                }
              }
            }

            previousDownloadStatesRef.current = newDownloadingMap;
          })
          .catch(console.error);
      };

      syncDownloadStatus();
      const interval = setInterval(syncDownloadStatus, 2000);
      return () => clearInterval(interval);
    }, []);

  const handleFileChange = (e: React.ChangeEvent<HTMLInputElement>) => {
    const file = e.target.files?.[0];
    if (file) {
      const reader = new FileReader();
      reader.onloadend = () => {
        setAttachedImage(reader.result as string);
      };
      reader.readAsDataURL(file);
    }
  };

const handleModelDownload = async (e: React.MouseEvent, modelId: string) => {
      e.stopPropagation(); // prevent model selection jump
      if (downloadingModels[modelId]) return;
      
      setDownloadingModels(prev => ({ ...prev, [modelId]: true }));
  setDownloadProgressStates(prev => ({ ...prev, [modelId]: Math.max(prev[modelId] || 0, 1) }));
      
      try {
          const response = await backendRequest('/api/download', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ model: modelId })
          });
          const data = response.data;

          if (!response.ok) {
            const backendMsg = data?.error ? `: ${data.error}` : "";
            throw new Error(`Server returned ${response.status}${backendMsg}`);
          }

          if (data.status === "already_downloading") {
            // Model is already downloading, keep the downloading state
            console.log(`Model ${modelId} is already downloading`);
          } else if (data.status === "success") {
            // Download completed immediately (e.g., file already exists)
            setDownloadingModels(prev => ({ ...prev, [modelId]: false }));
            setDownloadProgressStates(prev => ({ ...prev, [modelId]: 100 }));
            backendRequest('/api/models/local')
              .then(({ data }) => applyLocal(data))
              .catch(console.error);
          } else if (data.status !== "started") {
            setDownloadingModels(prev => ({ ...prev, [modelId]: false }));
            const backendMsg = data.error ? `\n\nBackend error: ${data.error}` : "";
            alert(`Unexpected download status: ${data.status}${backendMsg}`);
          }
      } catch (err) {
         console.error("Download failed:", err);
         setDownloadingModels(prev => ({ ...prev, [modelId]: false }));
         const message = err instanceof Error ? err.message : String(err);
         alert(`Download request failed.\n\n${message}`);
      }
  };

  const handleModelCancel = async (e: React.MouseEvent, modelId: string) => {
        e.preventDefault();
        e.stopPropagation();
      try {
          await backendRequest('/api/download/cancel', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ model: modelId })
          });
          // Immediately update frontend state instead of waiting for WebSocket
          setDownloadingModels(prev => ({ ...prev, [modelId]: false }));
          setDownloadProgressStates(prev => {
              const newState = {...prev};
              delete newState[modelId];
              return newState;
          });
      } catch (err) {
         console.error("Cancel failed:", err);
      }
  };

  const handleModelDelete = async (e: React.MouseEvent, modelId: string) => {
      e.stopPropagation();
      if (!confirm(`Are you sure you want to delete ${modelId}?`)) return;
      try {
        const res = await backendRequest(`/api/models/${encodeURIComponent(modelId)}`, {
              method: 'DELETE'
          });
          if (res.ok) {
              setLocalModels(prev => prev.filter(id => id !== modelId));
          } else {
              alert("Failed to delete model cache.");
          }
      } catch (err) {
          console.error("Delete failed:", err);
      }
  };

  const handleModelResetCache = async (e: React.MouseEvent, modelId: string) => {
      e.stopPropagation();
      if (!confirm(`Reset local cache for ${modelId}? This removes partial and downloaded files for this model.`)) return;
      try {
        const res = await backendRequest(`/api/models/${encodeURIComponent(modelId)}/reset-cache`, {
          method: 'POST'
        });
        if (res.ok) {
          setDownloadingModels(prev => ({ ...prev, [modelId]: false }));
          setDownloadProgressStates(prev => {
            const next = { ...prev };
            delete next[modelId];
            return next;
          });
          setLocalModels(prev => prev.filter(id => id !== modelId));
          const refreshed = await backendRequest('/api/models/local');
          applyLocal(refreshed.data);
        } else {
          alert(`Failed to reset cache for ${modelId}.`);
        }
      } catch (err) {
        console.error('Reset cache failed:', err);
        const message = err instanceof Error ? err.message : String(err);
        alert(`Reset cache failed.\n\n${message}`);
      }
  };

  const runConnectivityDiagnostics = async () => {
    setDiagnosticLoading(true);
    setDiagnosticResult(null);
    try {
      const res = await backendRequest('/api/diagnostics/connectivity');
      if (!res.ok) {
        throw new Error(`Server returned ${res.status}`);
      }
      const data = res.data || {};
      const lines = [
        data.summary ? `Summary: ${data.summary}` : 'Summary: unavailable',
        '',
        ...(data.results || []).map((row: any) => {
          if (row.ok) {
            return `${row.name}: OK (${row.status || '200'})${row.got_byte ? ' byte-read-ok' : ''}`;
          }
          return `${row.name}: FAIL${row.error ? ` - ${row.error}` : ''}`;
        }),
      ];
      setDiagnosticResult(lines.join('\n'));
    } catch (err) {
      const message = err instanceof Error ? err.message : String(err);
      setDiagnosticResult(`Diagnostics failed:\n${message}`);
    } finally {
      setDiagnosticLoading(false);
    }
  };

  const allModels = [
    ...(libraryModels.image || []).map((m: any) => ({...m, task: 'image'})),
    ...(libraryModels.video || []).map((m: any) => ({...m, task: 'video'})),
    ...(libraryModels.image_upscale || []).map((m: any) => ({...m, task: 'image_upscale'})),
    ...(libraryModels.video_upscale || []).map((m: any) => ({...m, task: 'video_upscale'}))
  ];

  const searchTask = taskMode === 'generate' ? mediaType : mediaType + "_upscale";
  useEffect(() => {
    // When searchTask changes (via toggling generation/upscale or image/video),
    // move to a model in the new category.
    //
    // Prefer one that is actually on disk. The registry is ordered smallest
    // first, so "the first one" is always the lightest tier - and picking it
    // when the user has a different model downloaded and none of this one lands
    // them on a dead send button with a download prompt, having chosen nothing.
    // The lightest tier is still the fallback when nothing is downloaded, which
    // is the right answer on a first run.
    const available = allModels.filter(m => m.task === searchTask);
    if (available.length > 0 && selectedModel.task !== searchTask) {
        const pick = available.find(m => localModels.includes(m.id)) ?? available[0];
        setSelectedModel({ id: pick.id, name: pick.name, task: pick.task });
    }
  }, [searchTask, allModels, selectedModel.task, localModels]);

  // Same idea for the very first render, which the effect above never covers:
  // it only fires when the task CHANGES, and the initial selection is a
  // hardcoded literal that may well not be downloaded.
  const initialPickDone = useRef(false);
  useEffect(() => {
    if (initialPickDone.current) return;
    const available = allModels.filter(m => m.task === searchTask);
    if (available.length === 0 || localModels.length === 0) return;
    initialPickDone.current = true;
    if (localModels.includes(selectedModel.id)) return;
    const pick = available.find(m => localModels.includes(m.id));
    if (pick) setSelectedModel({ id: pick.id, name: pick.name, task: pick.task });
  }, [allModels, localModels, searchTask, selectedModel.id]);

  // Adopt the selected model's own defaults.
  //
  // These are not cosmetic. Wan 2.2 TI2V 5B at our previous generic defaults
  // (320x320, guidance 7.5, no negative prompt) produced pure colour noise; at
  // its documented 480x832 / cfg 6.0 with the standard Wan negative prompt it
  // produced photorealistic video. The registry carries those numbers per
  // model, so switching models moves the controls with them.
  useEffect(() => {
    const m = allModels.find(x => x.id === selectedModel.id);
    if (!m) return;
    setSettings(prev => ({
      ...prev,
      ...(m.default_width  ? { width:  m.default_width  } : {}),
      ...(m.default_height ? { height: m.default_height } : {}),
      ...(typeof m.default_guidance === 'number' ? { guidance: m.default_guidance } : {}),
      ...(m.default_fps ? { fps: m.default_fps } : {}),
      // Only prefill the negative prompt while the user has not written one -
      // silently replacing something they typed would be worse than no default.
      ...(m.default_negative_prompt && !prev.negativePrompt
            ? { negativePrompt: m.default_negative_prompt } : {}),
    }));
    // Keyed on the model id alone: re-running whenever allModels is rebuilt
    // would fight the user every render.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [selectedModel.id]);

  // Which advanced controls the selected model actually supports.
  //
  // The server's model registry is the single source of truth here: a control
  // is rendered only when there is a real implementation behind it for this
  // model. That is why gpu_id and tile_size appear for generation but not for
  // upscaling (visp cannot address a specific device and hardcodes its tile
  // size), and tta_mode the other way round. Previously every one of these was
  // shown for upscaling and none of them did anything at all.
  const advancedCaps: string[] = (() => {
    const entry = allModels.find((m: any) => m.id === selectedModel.id && m.task === selectedModel.task);
    return Array.isArray(entry?.advanced) ? entry.advanced : [];
  })();
  const supportsAdvanced = (key: string) => advancedCaps.includes(key);

  // Video length is expressed as a duration because that is what anyone
  // actually wants to choose; frames are derived. The snap matters: a video
  // VAE compresses time (4:1 on Wan), and stable-diffusion.cpp rounds any
  // frame count DOWN to that grid without telling anyone - so asking for 48
  // frames silently yields 45. Snapping here means the duration shown is the
  // duration produced.
  const selectedEntry: any = allModels.find(
    (m: any) => m.id === selectedModel.id && m.task === selectedModel.task);
  const frameAlignment: number = Number(selectedEntry?.frame_alignment) > 0
    ? Number(selectedEntry.frame_alignment) : 1;
  // Every model until MiniMax H3 snaps to k*alignment + 1 (Wan's 4n+1). H3's
  // VAE instead wants 17k+5 (docs/minimax_h3.md in stable-diffusion.cpp) - same
  // shape, different offset - so the offset is now a registry field rather
  // than a hardcoded 1.
  const frameAlignmentOffset: number = Number(selectedEntry?.frame_alignment_offset) > 0
    ? Number(selectedEntry.frame_alignment_offset) : 1;

  const alignFrames = (frames: number) => {
    const n = Math.max(1, Math.round(frames));
    if (frameAlignment <= 1) return n;
    // Valid counts are k*alignment + offset; pick the nearest, never below the offset itself.
    const k = Math.max(0, Math.round((n - frameAlignmentOffset) / frameAlignment));
    return k * frameAlignment + frameAlignmentOffset;
  };

  const videoFps = Math.max(1, Number(settings.fps) || 16);
  const requestedFrames = Math.max(1, Math.round((Number(settings.videoDuration) || 0) * videoFps));
  const alignedFrames = alignFrames(requestedFrames);
  const actualDuration = alignedFrames / videoFps;

  const RATIOS: Record<string, number> = {
    '16:9': 16 / 9, '4:3': 4 / 3, '1:1': 1, '3:4': 3 / 4, '9:16': 9 / 16,
  };

  // The size the request will actually carry. Derived rather than computed at
  // submit time so the estimate below describes the run the user is about to
  // start, not an approximation of it.
  const plannedSize = (() => {
    const ratio = RATIOS[aspectRatio];
    if (!ratio) return { width: settings.width, height: settings.height };
    if (selectedModel.task === 'video') {
      // Video cannot reuse the image presets: those are all 1024-based, and a
      // 1024x576 video is ~1.5x the pixels of Wan's documented 480x832 -
      // measured at ~15 minutes a clip on a 6GB card, if the VAE fits at all.
      // Keep the model's own pixel budget, redistribute across the ratio.
      const round16 = (n: number) => Math.max(16, Math.round(n / 16) * 16);
      const targetPixels =
        (selectedEntry?.default_width ?? 480) * (selectedEntry?.default_height ?? 832);
      return {
        width:  round16(Math.sqrt(targetPixels * ratio)),
        height: round16(Math.sqrt(targetPixels / ratio)),
      };
    }
    if (aspectRatio === '16:9') return { width: 1024, height: 576 };
    if (aspectRatio === '4:3')  return { width: 1024, height: 768 };
    if (aspectRatio === '1:1')  return { width: 1024, height: 1024 };
    if (aspectRatio === '3:4')  return { width: 768,  height: 1024 };
    if (aspectRatio === '9:16') return { width: 576,  height: 1024 };
    return { width: settings.width, height: settings.height };
  })();

  const estimatedSeconds = estimateSeconds(
    runHistory, selectedModel.task, selectedModel.id,
    plannedSize.width * plannedSize.height,
    selectedModel.task === 'video' ? alignedFrames : 1,
    Math.max(1, settings.steps),
  );

  // Long enough that starting it by accident wastes real time. Deliberately a
  // time threshold rather than a frame cap: a frame cap tuned on one machine
  // would either block fast hardware needlessly or wave through a six-hour run
  // on slow hardware.
  const LONG_RUN_SECONDS = 10 * 60;
  const isLongRun = estimatedSeconds !== null && estimatedSeconds > LONG_RUN_SECONDS;

    const filteredModels = allModels.filter(m =>
    m.task === searchTask &&
    (m.name.toLowerCase().includes(searchModel.toLowerCase()) ||
    m.id.toLowerCase().includes(searchModel.toLowerCase()))
  ).sort((a, b) => {
     const aLocal = localModels.includes(a.id);
     const bLocal = localModels.includes(b.id);
     if (aLocal && !bLocal) return -1;
     if (!aLocal && bLocal) return 1;
     return a.name.localeCompare(b.name);
  });

  // The backend decides the container (VP9/WebM where libvpx is available,
  // MPEG-4 otherwise), so the UI reads the extension off the URL rather than
  // assuming one. The cache-busting query string has to be stripped first.
  const outputExtension = (url: string) => {
    const clean = url.split('?')[0];
    const dot = clean.lastIndexOf('.');
    return dot === -1 ? '' : clean.slice(dot).toLowerCase();
  };
  const isVideoOutput = (url: string) =>
    ['.webm', '.mp4', '.mkv', '.mov'].includes(outputExtension(url));

  const openNotices = async () => {
    const res = await window.vison?.notices?.();
    setNoticesText(res?.success ? (res.text ?? '') : (res?.error ?? 'Notices unavailable.'));
  };

  const handleDownloadImage = async (url: string) => {
    try {
      const resp = await fetch(url);
      const blob = await resp.blob();
      const objUrl = URL.createObjectURL(blob);
      const link = document.createElement('a');
      link.href = objUrl;
      // Was hardcoded to .png, which saved every generated video under a name
      // that no player would open.
      link.download = `vison-output-${Date.now()}${outputExtension(url) || '.png'}`;
      document.body.appendChild(link);
      link.click();
      document.body.removeChild(link);
      URL.revokeObjectURL(objUrl);
    } catch (err) {
      console.error(err);
    }
  };

  const handleSubmit = async (e?: React.FormEvent | React.KeyboardEvent) => {
      if (e) e.preventDefault();
    if ((!prompt.trim() && !attachedImage) || isGenerating) return;

    if (!localModels.includes(selectedModel.id)) {
        alert("Please download this model before generating!");
        return;
    }

    if (selectedModel.task === 'image_upscale' && !attachedImage) {
      alert('Please attach an image to upscale.');
      return;
    }

    // Nothing is blocked - the user's hardware decides what is reasonable, not
    // a cap chosen on ours. But a run measured in hours should be a decision,
    // not a slip of the Duration field.
    if (isLongRun && estimatedSeconds !== null) {
      const proceed = window.confirm(
        `This is about ${formatDuration(estimatedSeconds)} of generation ` +
        `(${alignedFrames} frames at ${plannedSize.width}×${plannedSize.height}).` +
        NL + NL +
        `The estimate comes from your last run on this machine. Start it?`);
      if (!proceed) return;
    }

    
      const { width: finalWidth, height: finalHeight } = plannedSize;

      const userMessage = { role: "user", content: prompt, baseImage: attachedImage || undefined };
    setMessages(prev => [...prev, userMessage]);
    setPrompt("");
    setAttachedImage(null);
    setIsGenerating(true);
    setProgress(-1);

    abortControllerRef.current = new AbortController();

    try {
      const response = await backendRequest('/api/generate', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        signal: abortControllerRef.current.signal,
        body: JSON.stringify({
          prompt: userMessage.content,
          model: selectedModel.id, 
          task: selectedModel.task,
          base_image: userMessage.baseImage,
          negative_prompt: settings.negativePrompt,
          num_inference_steps: settings.steps,
          guidance_scale: settings.guidance,
          width: finalWidth,
          height: finalHeight,
          seed: settings.seed,
          upscale_quality: settings.upscaleQuality,
          tile_size: settings.tileSize,
          tta_mode: settings.ttaMode,
          compression: settings.compression,
          output_format: settings.outputFormat,
          gpu_id: settings.gpuId,
          allow_fallback: settings.allowFallback,
          video_frames: alignedFrames,
          fps: videoFps
        })
      });

      if (!response.ok) {
        throw new Error(`Server returned ${response.status}`);
      }

      const data = response.data;
      if (data?.status === 'cancelled') {
        setMessages(prev => [...prev, { role: "assistant", content: "Generation cancelled by user." }]);
        return;
      }
      if (!data || data.status !== 'success') {
        throw new Error(data?.message || 'Generation failed on backend');
      }
      // Calibrate future estimates against what this machine actually did.
      // The backend reports its own elapsed time, which excludes the request
      // round trip and is the number worth learning from.
      if (typeof data.elapsed_seconds === 'number' && data.elapsed_seconds > 0) {
        const rec: RunRecord = {
          task: selectedModel.task,
          model: selectedModel.id,
          pixels: finalWidth * finalHeight,
          frames: selectedModel.task === 'video' ? alignedFrames : 1,
          steps: Math.max(1, settings.steps),
          seconds: data.elapsed_seconds,
          at: Date.now(),
        };
        recordRun(rec);
        setRunHistory(prev => [rec, ...prev].slice(0, RUN_HISTORY_MAX));
      }

      setMessages(prev => [...prev, {
        role: "assistant",
        content: `Here is your generated ${selectedModel.task}.`,
        url: data.image_url ? `${data.image_url}?t=${Date.now()}` : ""
      }]);
    } catch (err: any) {
      if (err.name === 'AbortError') {
         setMessages(prev => [...prev, { role: "assistant", content: "Generation cancelled by user." }]);
      } else {
        const reason = err instanceof Error ? err.message : String(err);
        setMessages(prev => [...prev, { role: "assistant", content: `Generation failed: ${reason}` }]);
      }
    } finally {
      setIsGenerating(false);
      setProgress(0);
    }
  };

  return (
    <div className="flex flex-col h-screen text-gray-100 font-sans relative" style={{ backgroundColor: '#181818' }}>

      {/* Backend Offline Overlay */}
      {isConnected === false && (
         <div className="absolute inset-0 z-[100] bg-black/90 backdrop-blur-md flex flex-col items-center justify-center p-8 text-center" style={{ WebkitAppRegion: 'drag' } as any}>
            <div className="bg-red-500/10 border border-red-500/20 p-6 rounded-2xl max-w-md w-full" style={{ WebkitAppRegion: 'no-drag' } as any}>
               <h2 className="text-xl font-bold text-red-500 mb-2 animate-pulse">Backend Offline</h2>
               <p className="text-gray-300 text-sm mb-4">Vison could not connect to the backend server running on port 11439. Please check the terminal for errors or wait for startup.</p>
               <Loader2 className="w-8 h-8 text-red-500 animate-spin mx-auto opacity-50" />
            </div>
         </div>
      )}

      {/* Header */}
        <header className="flex items-center justify-between pl-6 py-4 border-b border-[#343434] select-none z-50 relative bg-[#181818]"
                style={{
                  WebkitAppRegion: 'drag',
                  // main.ts uses titleBarStyle 'hidden' with a titleBarOverlay, so
                  // Windows paints minimise/maximise/close ON TOP of the top-right
                  // of the page. Anything the header puts there is unreachable and
                  // half-hidden - which is exactly what happened to the sign-out
                  // button. Chromium exposes the strip it reserved as
                  // env(titlebar-area-*); the fallback is the default Windows
                  // control width, for the case where those are unavailable.
                  paddingRight:
                    'calc(100vw - env(titlebar-area-width, calc(100vw - 138px))' +
                    ' - env(titlebar-area-x, 0px) + 12px)',
                } as any}>
          <div className="flex items-center gap-4 text-gray-400" style={{ WebkitAppRegion: 'no-drag' } as any}>
            <span title="Toggle Chat History" className="inline-flex">
              <History
                 className={`w-5 h-5 cursor-pointer transition-colors ${showSidebar ? 'text-white' : 'hover:text-white'}`}
                 onClick={() => setShowSidebar(!showSidebar)}
              />
            </span>
            <span title="Model Library" className="inline-flex">
              <Library
                 className={`w-5 h-5 cursor-pointer transition-colors ${currentView === 'library' ? 'text-white' : 'hover:text-white'}`}
                 onClick={() => setCurrentView(currentView === 'library' ? 'chat' : 'library')}
              />
            </span>
            {currentView === 'chat' && !selectedModel.task.includes("upscale") && (
              <>
                <Settings
                  className={`w-5 h-5 cursor-pointer transition-colors ${showSettings ? 'text-white' : 'hover:text-white'}`}
                  onClick={() => setShowSettings(!showSettings)}
                />
                <span title="New Chat" className="inline-flex"><PenBox className="w-5 h-5 cursor-pointer hover:text-white transition-colors" onClick={createNewChat} /></span>
              </>
            )}
            {currentView === 'chat' && selectedModel.task.includes("upscale") && (
                <span title="New Chat" className="inline-flex"><PenBox className="w-5 h-5 cursor-pointer hover:text-white transition-colors" onClick={createNewChat} /></span>
            )}
          </div>
          {/* Account menu.
              Always rendered, signed in or not, and always carrying the same
              three items. It used to appear only with a session, which was fine
              when a session was mandatory - now that sign-in is optional it
              would have hidden Settings and Support Vison from most users, since
              most will never sign in. */}
          <div className="relative" ref={accountMenuRef}
               style={{ WebkitAppRegion: 'no-drag' } as any}>
            <button
              type="button"
              onClick={() => setShowAccountMenu(v => !v)}
              aria-haspopup="menu"
              aria-expanded={showAccountMenu}
              title={authUser ? authUser.email : 'Account'}
              className={`flex items-center gap-2 rounded-full py-1 pl-1 pr-2 text-xs transition-colors ${
                showAccountMenu ? 'bg-[#2c2c2c] text-white' : 'text-gray-400 hover:bg-[#2c2c2c] hover:text-white'}`}
            >
              {authUser?.picture ? (
                <img src={authUser.picture} alt="" className="h-7 w-7 rounded-full object-cover"
                     onError={e => { (e.currentTarget as HTMLImageElement).style.display = 'none'; }} />
              ) : (
                <span className="flex h-7 w-7 items-center justify-center rounded-full bg-[#3a3a3a] text-[11px] font-medium text-gray-200">
                  {authUser
                    ? (authUser.name || authUser.email || '?').trim().charAt(0).toUpperCase()
                    : <User className="h-3.5 w-3.5" />}
                </span>
              )}
              <span className="max-w-[14ch] truncate font-medium">
                {authUser ? (authUser.name || authUser.email) : 'Account'}
              </span>
              <ChevronDown className={`h-3.5 w-3.5 shrink-0 transition-transform ${showAccountMenu ? 'rotate-180' : ''}`} />
            </button>

            {showAccountMenu && (
              <div role="menu"
                   className="absolute right-0 top-full z-50 mt-2 w-64 overflow-hidden rounded-xl border border-[#3a3a3a] bg-[#232323] shadow-xl shadow-black/40">
                <div className="border-b border-[#343434] px-3 py-2.5">
                  {authUser ? (
                    <>
                      <p className="truncate text-sm font-medium text-gray-100">{authUser.name || 'Signed in'}</p>
                      <p className="truncate text-xs text-gray-500" title={authUser.email}>{authUser.email}</p>
                    </>
                  ) : (
                    <>
                      <p className="text-sm font-medium text-gray-100">Not signed in</p>
                      <p className="mt-0.5 text-xs text-gray-500">
                        {authConfigured
                          ? 'Everything works without it. Signing in is optional.'
                          : 'This build was made without sign-in configured.'}
                      </p>
                    </>
                  )}
                </div>

                {/* Signed out, the first item is the one the header just talked
                    about, so it leads. Signed in, sign-out is the last thing
                    you want under the cursor by accident, so it trails. */}
                {!authUser && (
                  <button
                    type="button"
                    role="menuitem"
                    onClick={handleSignIn}
                    disabled={authBusy || !authConfigured}
                    title={authConfigured ? undefined : 'This build has no Google client configured'}
                    className="flex w-full items-center gap-2.5 px-3 py-2 text-left text-sm text-gray-300 transition-colors hover:bg-[#2c2c2c] hover:text-white disabled:cursor-not-allowed disabled:text-gray-600 disabled:hover:bg-transparent"
                  >
                    {authBusy
                      ? <Loader2 className="h-4 w-4 shrink-0 animate-spin" />
                      : <LogIn className="h-4 w-4 shrink-0" />}
                    {authBusy ? 'Waiting for your browser…' : 'Log in'}
                  </button>
                )}

                <button
                  type="button"
                  role="menuitem"
                  onClick={openSettingsFromMenu}
                  className="flex w-full items-center gap-2.5 px-3 py-2 text-left text-sm text-gray-300 transition-colors hover:bg-[#2c2c2c] hover:text-white"
                >
                  <Settings className="h-4 w-4 shrink-0" />
                  Settings
                </button>

                <button
                  type="button"
                  role="menuitem"
                  onClick={() => { setShowAccountMenu(false); setShowSupport(true); }}
                  className="flex w-full items-center gap-2.5 px-3 py-2 text-left text-sm text-gray-300 transition-colors hover:bg-[#2c2c2c] hover:text-white"
                >
                  <Heart className="h-4 w-4 shrink-0" />
                  Support Vison
                </button>

                {authUser && (
                  <button
                    type="button"
                    role="menuitem"
                    onClick={() => { setShowAccountMenu(false); handleSignOut(); }}
                    className="flex w-full items-center gap-2.5 border-t border-[#343434] px-3 py-2 text-left text-sm text-gray-300 transition-colors hover:bg-[#2c2c2c] hover:text-red-400"
                  >
                    <LogOut className="h-4 w-4 shrink-0" />
                    Sign out
                  </button>
                )}

                {authError && (
                  <p className="border-t border-[#343434] px-3 py-2 text-xs text-red-400">{authError}</p>
                )}
              </div>
            )}
          </div>
      </header>

      {/* Sidebar UI */}
      <div className={`absolute top-[65px] left-0 bottom-0 bg-[#1e1e1e] border-r border-[#343434] z-40 transition-transform duration-300 w-64 flex flex-col ${showSidebar ? 'translate-x-0' : '-translate-x-full'}`}>
         <div className="p-4 border-b border-[#343434] flex justify-between items-center">
            <h3 className="font-semibold text-gray-200">Chat History</h3>
            <button onClick={createNewChat} className="p-1.5 bg-[#303030] hover:bg-white hover:text-black rounded-lg transition-colors" title="New Chat">
               <Plus className="w-4 h-4" />
            </button>
         </div>
         {/* Search runs over the full text of every conversation, not the
             titles in this list - the whole point is to find a chat by
             something said inside it. */}
         <div className="px-3 pt-3 pb-1">
            <div className="relative">
               <Search className="pointer-events-none absolute left-2.5 top-1/2 h-3.5 w-3.5 -translate-y-1/2 text-gray-500" />
               <input
                  type="text"
                  value={chatQuery}
                  onChange={e => setChatQuery(e.target.value)}
                  onKeyDown={e => { if (e.key === 'Escape') setChatQuery(''); }}
                  placeholder="Search chats"
                  aria-label="Search chats"
                  className="w-full rounded-lg border border-transparent bg-[#2a2a2a] py-1.5 pl-8 pr-7 text-xs text-gray-200 placeholder:text-gray-500 outline-none transition-colors focus:border-[#4a4a4a] focus:bg-[#303030]"
               />
               {chatQuery && (
                  <button
                     type="button"
                     onClick={() => setChatQuery('')}
                     aria-label="Clear search"
                     className="absolute right-1.5 top-1/2 -translate-y-1/2 rounded p-1 text-gray-500 transition-colors hover:text-white"
                  >
                     <X className="h-3 w-3" />
                  </button>
               )}
            </div>
         </div>

         <div className="flex-1 overflow-y-auto p-2 flex flex-col gap-1">
            {chatResults === null ? (
              <>
                {conversations.map(c => (
                   <div key={c.id} onClick={() => loadConversation(c.id)} className={`group relative p-3 rounded-xl cursor-pointer transition-colors ${currentConversationId === c.id ? 'bg-[#303030] text-white' : 'hover:bg-[#2a2a2a] text-gray-400'}`}>
                      <p className="text-sm truncate pr-6 font-medium">{c.title || "Image Generation"}</p>
                      <p className="text-[10px] opacity-50 mt-1">{new Date(c.timestamp).toLocaleString()}</p>
                      <button onClick={(e) => deleteConversation(e, c.id)} className="absolute right-2 top-1/2 -translate-y-1/2 p-1.5 opacity-0 group-hover:opacity-100 hover:text-red-500 transition-all bg-[#1e1e1e] group-hover:bg-[#2a2a2a] rounded">
                         <Trash2 className="w-3.5 h-3.5" />
                      </button>
                   </div>
                ))}
                {conversations.length === 0 && <p className="text-xs text-center text-gray-500 mt-4">No history yet</p>}
              </>
            ) : chatResults.length === 0 ? (
              <p className="mt-4 text-center text-xs text-gray-500">No chats match “{chatQuery.trim()}”</p>
            ) : (
              <>
                <p className="px-1 pb-1 text-[10px] uppercase tracking-wide text-gray-600">
                   {chatResults.length} {chatResults.length === 1 ? 'match' : 'matches'}
                </p>
                {chatResults.map(hit => (
                   <div key={hit.id} onClick={() => loadConversation(hit.id)} className={`group relative p-3 rounded-xl cursor-pointer transition-colors ${currentConversationId === hit.id ? 'bg-[#303030] text-white' : 'hover:bg-[#2a2a2a] text-gray-400'}`}>
                      <p className="text-sm truncate pr-6 font-medium">{hit.title || "Image Generation"}</p>
                      {hit.parts.length > 0 && (
                         <p className="mt-1 line-clamp-2 text-[11px] leading-snug text-gray-500">
                            {hit.parts.map((part, i) =>
                               part.match
                                  ? <mark key={i} className="rounded bg-amber-400/20 px-0.5 text-amber-200">{part.text}</mark>
                                  : <span key={i}>{part.text}</span>
                            )}
                         </p>
                      )}
                      <p className="text-[10px] opacity-50 mt-1">{new Date(hit.timestamp).toLocaleString()}</p>
                      <button onClick={(e) => deleteConversation(e, hit.id)} className="absolute right-2 top-2 p-1.5 opacity-0 group-hover:opacity-100 hover:text-red-500 transition-all bg-[#1e1e1e] group-hover:bg-[#2a2a2a] rounded">
                         <Trash2 className="w-3.5 h-3.5" />
                      </button>
                   </div>
                ))}
              </>
            )}
         </div>
      </div>

      {/* Main Area */}
      <main className={`flex-1 overflow-y-auto w-full max-w-4xl mx-auto flex flex-col pt-8 transition-all duration-300 ${showSidebar ? 'ml-64' : ''}`}>
        
        {/* Library View */}
        {currentView === 'library' ? (
           <div className="px-6 pb-20">
              <div className="flex gap-4 mb-8">
                 {['image', 'image_upscale', 'video', 'video_upscale'].map((tab) => (
                    <button 
                       key={tab}
                       className={`px-4 py-2 rounded-full capitalize font-medium text-sm transition-all flex items-center gap-2 ${activeTab === tab ? "bg-white text-black" : "text-gray-400 hover:text-white"}`}
                       onClick={() => setActiveTab(tab)}
                    >
                       {tab.includes('image') && <ImageIcon className="w-4 h-4"/>}
                       {tab.includes('video') && <Video className="w-4 h-4"/>}
                       {tab.includes('upscale') && <Maximize className="w-4 h-4"/>}
                       {tab.replace('_', ' ')}
                    </button>
                 ))}
              </div>
              <div className="grid grid-cols-1 md:grid-cols-2 gap-4">
                 {(libraryModels as any)[activeTab]?.map((model: any) => {
                    const total = modelBytes(model);
                    const onDisk = presentBytes[model.id] || 0;
                    const remaining = Math.max(0, total - onDisk);
                    const have = localModels.includes(model.id);
                    const busy = !!downloadingModels[model.id];
                    const pct = downloadProgressStates[model.id] || 0;
                    const isActive = selectedModel.id === model.id;
                    return (
                    <div key={model.id} className={`flex flex-col bg-[#242424] border p-5 rounded-2xl transition-all group ${isActive ? 'border-white/40' : 'border-[#343434] hover:border-[#4f4f4f]'}`}>
                       <div className="flex justify-between items-start gap-3 mb-2">
                          <h3 className="font-semibold text-lg leading-tight">{model.name}</h3>
                          <span className="text-xs bg-[#181818] px-2 py-1 rounded-md text-gray-400 whitespace-nowrap flex-shrink-0">{formatSize(model)}</span>
                       </div>
                       <p className="text-sm text-gray-400 mb-4">{model.description}</p>
                       <CompatibilityNote compatibility={model.compatibility} />

                       {/* Push the actions to the bottom so cards in a row line
                           up regardless of how long the description runs. */}
                       <div className="mt-auto pt-4 border-t border-[#303030] flex flex-col gap-3">
                          {busy ? (
                             <div className="flex flex-col gap-2">
                                <div className="flex items-center justify-between text-xs">
                                   <span className="flex items-center gap-2 text-blue-400">
                                      <Loader2 className="w-3.5 h-3.5 animate-spin" />
                                      Downloading {formatBytes(remaining)}
                                   </span>
                                   <span className="font-mono text-blue-400">{pct}%</span>
                                </div>
                                <div className="w-full h-1.5 bg-[#181818] rounded-full overflow-hidden">
                                   <div className="bg-blue-500 h-full transition-all" style={{ width: `${pct}%` }} />
                                </div>
                                <button
                                   type="button"
                                   className="self-start text-xs text-gray-500 hover:text-red-400 transition-colors flex items-center gap-1"
                                   onClick={(e) => handleModelCancel(e, model.id)}
                                >
                                   <X className="w-3 h-3" /> Cancel
                                </button>
                             </div>
                          ) : have ? (
                             <div className="flex items-center justify-between text-xs">
                                <span className="flex items-center gap-2 text-green-500">
                                   <CheckCircle2 className="w-3.5 h-3.5" /> On this machine
                                </span>
                                <button
                                   type="button"
                                   className="text-gray-500 hover:text-red-400 transition-colors flex items-center gap-1"
                                   onClick={(e) => handleModelDelete(e, model.id)}
                                >
                                   <Trash2 className="w-3.5 h-3.5" /> Remove
                                </button>
                             </div>
                          ) : (
                             <div className="flex items-center justify-between gap-3">
                                <span className="text-xs text-gray-500">
                                   {/* Models share files - the same Qwen2.5-VL encoder backs
                                       Qwen-Image and HunyuanVideo, the same VAE backs Z-Image
                                       and FLUX - and the downloader skips whatever already
                                       verifies. Quoting the full size when most of it is
                                       already here makes a reachable model look impossible. */}
                                   {onDisk > 0
                                      ? <>{formatBytes(remaining)} to download <span className="text-gray-600">· {formatBytes(onDisk)} already here</span></>
                                      : <>{formatBytes(total)} to download</>}
                                </span>
                                <button
                                   type="button"
                                   className="flex items-center gap-1.5 text-xs font-medium px-3 py-1.5 rounded-lg bg-[#303030] hover:bg-white hover:text-black text-gray-200 transition-colors flex-shrink-0"
                                   onClick={(e) => handleModelDownload(e, model.id)}
                                >
                                   <Download className="w-3.5 h-3.5" /> Download
                                </button>
                             </div>
                          )}

                          <button
                             className={`w-full py-2 rounded-lg text-sm font-medium transition-colors ${
                                isActive
                                   ? 'bg-[#1f1f1f] text-gray-500 cursor-default'
                                   : model.compatibility?.fit === 'unsupported'
                                      ? 'bg-[#2a2020] text-gray-500 hover:bg-[#3a2626] hover:text-gray-300'
                                      : 'bg-[#303030] hover:bg-white hover:text-black text-gray-200'
                             }`}
                             onClick={() => {
                                setSelectedModel({ id: model.id, name: model.name, task: activeTab });
                                setCurrentView('chat');
                             }}
                          >
                             {isActive
                                ? 'Selected'
                                : model.compatibility?.fit === 'unsupported'
                                   ? 'Select Anyway'
                                   : 'Select Model'}
                          </button>
                       </div>
                    </div>
                    );
                 })}
              </div>

              {/* Attribution has to be reachable, not just present on disk -
                  every licence here requires the notice to travel with the
                  distribution, and a file nobody can find does not satisfy
                  that. The library view is the one screen always available. */}
              <div className="mt-10 pt-6 border-t border-[#2c2c2c] text-xs text-gray-500">
                Vison uses open-source components.{' '}
                <button
                  type="button"
                  onClick={openNotices}
                  className="underline underline-offset-2 hover:text-gray-300 transition-colors"
                >
                  Third-party licences
                </button>
              </div>
           </div>
        ) : (
        /* Chat View */
        messages.length === 0 ? (
          <div className="flex flex-col items-center justify-center flex-1 space-y-4 pb-20">
            <div className="w-20 h-20 bg-white rounded-full flex items-center justify-center mb-4 shadow-xl overflow-hidden">
              <img src={visonLogo} alt="Vison" className="w-16 h-16 object-contain" />
            </div>
            <h1 className="text-xl font-semibold opacity-80 text-gray-400">Ready to synthesize</h1>
          </div>
        ) : (
          <div className="flex-1 space-y-8 px-4 pb-40">
            {messages.map((msg, idx) => (
              <div key={idx} className={`w-full flex ${msg.role === 'user' ? 'justify-end' : 'justify-start'}`}>
                <div className={`p-4 rounded-3xl max-w-xl ${msg.role === 'user' ? 'bg-[#303030] text-white' : 'bg-transparent text-gray-200'}`}>
                  {msg.baseImage && (
                     <img src={msg.baseImage} alt="Base" className="w-32 h-32 object-cover rounded-xl mb-3 opacity-80 border border-gray-600" />
                  )}
                  {msg.content && <p>{msg.content}</p>}
                  {msg.url && (
                      <div className="relative group mt-4 inline-block">
                        {/* Video used to render through this same <img>, which
                            meant every generated clip showed as a broken image.
                            Chromium plays VP9/WebM natively, so a <video> just
                            works here. */}
                        {isVideoOutput(msg.url) ? (
                          <video
                            src={msg.url}
                            controls
                            loop
                            muted
                            autoPlay
                            playsInline
                            className="rounded-xl shadow-md border border-[#303030] max-w-md w-full"
                          />
                        ) : (
                          <img src={msg.url} alt="Generated output" className="rounded-xl shadow-md border border-[#303030] max-w-md w-full object-cover" />
                        )}
                        <button
                          onClick={() => handleDownloadImage(msg.url || "")}
                          className="absolute top-2 right-2 p-2 bg-black/70 rounded-lg text-white opacity-0 group-hover:opacity-100 transition-opacity hover:bg-black/90 flex items-center gap-1 shadow"
                          title={isVideoOutput(msg.url) ? "Download video" : "Download image"}
                        >
                          <Download size={16} />
                        </button>
                      </div>
                    )}
                </div>
              </div>
            ))}
            {isGenerating && (
               <div className="flex justify-start">
                   <div className="p-4 bg-[#242424] border border-[#343434] rounded-3xl max-w-xl text-gray-400 w-64 shadow-lg">
                     <div className="flex justify-between mb-2 text-sm font-medium">
                        <span className="text-white">{progress === -1 ? 'Loading Model...' : 'Generating...'}</span>
                        <span className="text-gray-500">{progress === -1 ? '' : `${progress}%`}</span>
                     </div>
                     <div className="bg-[#181818] rounded-full h-1.5 overflow-hidden">
                       <div className={`bg-white h-1.5 transition-all duration-300 ${progress === -1 ? 'animate-pulse' : ''}`} style={{ width: `${progress === -1 ? 100 : progress}%` }}></div>
                     </div>
                   </div>
               </div>
            )}
            <div ref={messagesEndRef} />
          </div>
        ))}
      </main>

      {/* Input Area (Only show in Chat View) */}
      {currentView === 'chat' && (
        <div className="w-full absolute bottom-8 left-1/2 -translate-x-1/2 max-w-3xl px-4 flex flex-col gap-2">
          
          {/* Attachment Preview Bubble */}
          {attachedImage && (
             <div className="self-start relative group ml-2 mt-[-40px] z-10 transition-all">
                <img src={attachedImage} className="w-20 h-20 object-cover rounded-xl border-2 border-[#343434] shadow-2xl" />
                <button 
                  onClick={() => setAttachedImage(null)} 
                  className="absolute -top-2 -right-2 bg-gray-800 rounded-full p-1 border border-gray-600 hover:bg-red-500 hover:text-white"
                >
                   <X className="w-3 h-3" />
                </button>
             </div>
          )}

          {/* The send button is disabled until the selected model is on disk.
              Leaving that unexplained was fine when there were two models and
              you had both; with ten spanning 9 MB to 22 GB, the common case is
              picking one you have not fetched and finding a dead button with
              no stated reason. Say what is missing and offer the fix here,
              rather than making the user find the library screen again. */}
          {currentView === 'chat' && selectedEntry && !localModels.includes(selectedModel.id) && (
            <div className="flex items-center justify-between gap-3 rounded-2xl border border-[#3a3320] bg-[#2a2418]/80 px-4 py-3 backdrop-blur">
              <div className="min-w-0">
                <p className="text-sm text-amber-200/90 font-medium truncate">
                  {selectedModel.name} is not on this machine yet
                </p>
                <p className="text-xs text-gray-400 mt-0.5">
                  {(() => {
                    const total = modelBytes(selectedEntry);
                    const onDisk = presentBytes[selectedModel.id] || 0;
                    const remaining = Math.max(0, total - onDisk);
                    return onDisk > 0
                      ? `${formatBytes(remaining)} left to download — ${formatBytes(onDisk)} of it is already here`
                      : `${formatBytes(total)} to download`;
                  })()}
                </p>
              </div>
              {downloadingModels[selectedModel.id] ? (
                <div className="flex items-center gap-2 flex-shrink-0">
                  <Loader2 className="w-4 h-4 animate-spin text-blue-400" />
                  <span className="text-xs font-mono text-blue-400 w-8 text-right">
                    {downloadProgressStates[selectedModel.id] || 0}%
                  </span>
                  <button
                    type="button"
                    title="Cancel download"
                    className="p-1 rounded text-gray-500 hover:text-red-400 transition-colors"
                    onClick={(e) => handleModelCancel(e, selectedModel.id)}
                  >
                    <X className="w-4 h-4" />
                  </button>
                </div>
              ) : (
                <button
                  type="button"
                  className="flex items-center gap-1.5 text-xs font-medium px-3 py-2 rounded-lg bg-[#303030] hover:bg-white hover:text-black text-gray-200 transition-colors flex-shrink-0"
                  onClick={(e) => handleModelDownload(e, selectedModel.id)}
                >
                  <Download className="w-3.5 h-3.5" /> Download
                </button>
              )}
            </div>
          )}

          <form 
            onSubmit={handleSubmit}
            className="relative flex flex-col bg-[#242424] rounded-3xl border border-[#343434] focus-within:border-[#4f4f4f] transition-all pt-2 pb-2 pl-4 pr-2 shadow-2xl"
          >
            <textarea
              ref={textareaRef}
              className="w-full bg-transparent border-none text-white focus:outline-none placeholder-gray-500 resize-none overflow-y-auto mb-2 text-sm leading-relaxed"
              style={{ minHeight: '44px', maxHeight: '160px' }}
              rows={1}
              placeholder="Send a message"
              value={prompt}
              onChange={handleInput}
              onKeyDown={(e) => {
                  if (e.key === 'Enter' && !e.shiftKey) {
                      e.preventDefault();
                      handleSubmit();
                  }
              }}
            />
            
            <div className="flex items-center justify-between mt-1">
              <div className="flex items-center gap-2">
                <input type="file" hidden accept="image/*" ref={fileInputRef} onChange={handleFileChange} />
                
                <button 
                  type="button" 
                  onClick={() => fileInputRef.current?.click()}
                  className="w-8 h-8 flex items-center justify-center text-gray-400 hover:text-white bg-[#303030]/50 hover:bg-[#404040] rounded-full transition-colors"
                  title="Attach Base Image"
                >
                  <Plus className="w-5 h-5" />
                </button>
              </div>

              <div className="flex items-center gap-2 relative">
                {/* Unified Target Configuration Pill */}
                <button 
                  type="button" 
                  onClick={() => setShowModelDropdown(!showModelDropdown)} 
                  className="h-9 px-4 flex items-center gap-2 text-sm text-gray-300 font-medium hover:text-white bg-[#333333] hover:bg-[#404040] rounded-full transition-colors"
                >
                  {mediaType === 'image' ? "Image" : "Video"}
                  <span className="text-gray-500">•</span>
                  {shortModelName(selectedModel.name)}
                  <span className="text-gray-500 mx-1">•</span>
                  <span className="flex items-center gap-1.5 opacity-90"><RectangleHorizontal className="w-4 h-4"/> {aspectRatio}</span>
                  {taskMode === 'upscale' && (
                    <>
                       <span className="text-gray-500 mx-1">•</span>
                       <span className="flex items-center gap-1 opacity-90">x{settings.upscaleQuality.replace('x', '')}</span>
                    </>
                  )}
                </button>

                {showModelDropdown && (
                  <div
                    className="absolute bottom-[52px] right-0 w-[420px] max-w-[min(420px,calc(100vw-24px))] bg-[#1c1c1c] border border-[#2c2c2c] rounded-[24px] shadow-2xl p-4 flex flex-col gap-4 z-50 overflow-y-auto"
                    style={{ maxHeight: 'calc(100vh - 120px)' }}
                  >
                    
                    {/* Feature Toggles Rows */}
                    <div className="grid grid-cols-2 gap-2">
                      <div className="flex bg-[#2c2c2c] p-1 rounded-[16px]">
                          <button
                             type="button"
                             onClick={() => setMediaType('image')}
                             className={`flex-1 flex items-center justify-center gap-2 text-sm py-2 rounded-[12px] font-medium transition-colors ${mediaType === 'image' ? 'bg-[#3c3c3c] text-white shadow-sm' : 'text-gray-400 hover:text-gray-200'}`}
                          >
                             <ImageIcon className="w-4 h-4" /> Image
                          </button>
                          <button
                             type="button"
                             onClick={() => setMediaType('video')}
                             className={`flex-1 flex items-center justify-center gap-2 text-sm py-2 rounded-[12px] font-medium transition-colors ${mediaType === 'video' ? 'bg-[#3c3c3c] text-white shadow-sm' : 'text-gray-400 hover:text-gray-200'}`}
                          >
                             <Video className="w-4 h-4" /> Video
                          </button>
                      </div>

                      <div className="flex bg-[#2c2c2c] p-1 rounded-[16px]">
                          <button
                             type="button"
                             onClick={() => setTaskMode('generate')}
                             className={`flex-1 flex items-center justify-center gap-2 text-sm py-2 rounded-[12px] font-medium transition-colors ${taskMode === 'generate' ? 'bg-[#3c3c3c] text-white shadow-sm' : 'text-gray-400 hover:text-gray-200'}`}
                          >
                             <Sparkles className="w-4 h-4" /> Generate
                          </button>
                          <button
                             type="button"
                             onClick={() => setTaskMode('upscale')}
                             className={`flex-1 flex items-center justify-center gap-2 text-sm py-2 rounded-[12px] font-medium transition-colors ${taskMode === 'upscale' ? 'bg-[#3c3c3c] text-white shadow-sm' : 'text-gray-400 hover:text-gray-200'}`}
                          >
                             <Maximize2 className="w-4 h-4" /> Upscale
                          </button>
                      </div>
                    </div>

                    {/* Aspect Ratios */}
                    <div className="flex bg-[#323232] rounded-[16px] overflow-hidden divide-x divide-[#404040]">
                        {[
                          { label: '16:9', icon: <RectangleHorizontal className="w-4 h-4" /> },
                          { label: '4:3', icon: <Frame className="w-4 h-4" /> },
                          { label: '1:1', icon: <Square className="w-4 h-4" /> },
                          { label: '3:4', icon: <Columns className="w-4 h-4 rotate-90" /> },
                          { label: '9:16', icon: <RectangleVertical className="w-4 h-4" /> }
                        ].map(ratio => (
                           <button
                              key={ratio.label}
                              type="button"
                              onClick={() => setAspectRatio(ratio.label)}
                              className={`flex-1 flex flex-col items-center justify-center gap-1.5 py-3 transition-colors ${aspectRatio === ratio.label ? 'bg-[#4a4a4a] text-white' : 'hover:bg-[#3d3d3d] text-gray-400'}`}
                           >
                              {ratio.icon}
                              <span className="text-xs font-medium">{ratio.label}</span>
                           </button>
                        ))}
                    </div>

                    {/* Upscale Options */}
                    {taskMode === 'upscale' && (
                       <div className="flex bg-[#323232] rounded-[16px] overflow-hidden divide-x divide-[#404040]">
                           {['2x', '4x', '1080p', '1440p', '2160p'].map(factor => (
                              <button
                                 key={factor}
                                 type="button"
                                 onClick={() => setSettings({...settings, upscaleQuality: factor})}
                                 className={`flex-1 flex items-center justify-center py-3 text-sm transition-colors ${settings.upscaleQuality === factor ? 'bg-[#4a4a4a] font-semibold text-white' : 'hover:bg-[#3d3d3d] text-gray-400'}`}
                              >
                                 x{factor.replace('x', '')}
                              </button>
                           ))}
                       </div>
                    )}

                    {/* Quick Setting Adjustments */}
                     <div className="flex gap-2">
                         <div className="flex bg-[#2c2c2c] p-1.5 rounded-[12px] flex-1 items-center px-4">
                            <span className="text-gray-400 text-xs w-16">Steps</span>
                            <input type="number" className="min-w-0 w-16 flex-1 bg-transparent border-none text-white text-sm outline-none text-right" value={settings.steps} onChange={e => setSettings({...settings, steps: Number(e.target.value)})} />
                         </div>
                         <div className="flex bg-[#2c2c2c] p-1.5 rounded-[12px] flex-1 items-center px-4">
                            <span className="text-gray-400 text-xs w-16">Guidance</span>
                            <input type="number" step="0.5" className="min-w-0 w-16 flex-1 bg-transparent border-none text-white text-sm outline-none text-right" value={settings.guidance} onChange={e => setSettings({...settings, guidance: Number(e.target.value)})} />
                         </div>
                     </div>

                        {/* Duration, not frame count: nobody thinks in frames.
                            The derived count is still shown, because generation
                            cost scales with it and because the VAE's time
                            compression means only certain counts are legal. */}
                        {selectedModel.task === 'video' && (
                          <div className="space-y-2">
                            <div className="grid grid-cols-2 gap-2">
                              <div className="flex bg-[#2c2c2c] p-1.5 rounded-[12px] items-center px-4">
                                <span className="text-gray-400 text-xs w-20" title="How long the clip should be. Generation time scales with this.">Duration</span>
                                <input
                                  type="number"
                                  min="0.25"
                                  step="0.25"
                                  className="min-w-0 w-14 flex-1 bg-transparent border-none text-white text-sm outline-none text-right"
                                  value={settings.videoDuration}
                                  onChange={e => setSettings({...settings, videoDuration: Number(e.target.value)})}
                                />
                                <span className="text-gray-500 text-xs ml-1">s</span>
                              </div>
                              <div className="flex bg-[#2c2c2c] p-1.5 rounded-[12px] items-center px-4">
                                <span className="text-gray-400 text-xs w-20" title="Playback rate of the encoded mp4.">FPS</span>
                                <input
                                  type="number"
                                  min="1"
                                  className="min-w-0 w-14 flex-1 bg-transparent border-none text-white text-sm outline-none text-right"
                                  value={settings.fps}
                                  onChange={e => setSettings({...settings, fps: Number(e.target.value)})}
                                />
                              </div>
                            </div>
                            <p className="text-xs text-gray-500 px-1">
                              {alignedFrames} frames
                              {frameAlignment > 1 && alignedFrames !== requestedFrames && (
                                <> (snapped from {requestedFrames} — this model only accepts {frameAlignment}n+1)</>
                              )}
                              {Math.abs(actualDuration - (Number(settings.videoDuration) || 0)) > 0.01 && (
                                <> → {actualDuration.toFixed(2)}s</>
                              )}
                              {' · '}{plannedSize.width}×{plannedSize.height}
                            </p>
                            {/* Measured on this machine, not assumed. Until the
                                first video finishes there is nothing to scale
                                from, and saying so is better than guessing. */}
                            <p className={`text-xs px-1 ${isLongRun ? 'text-amber-400' : 'text-gray-500'}`}>
                              {estimatedSeconds === null
                                ? 'Generation time unknown until the first clip finishes on this machine.'
                                : <>Roughly <span className="font-medium">{formatDuration(estimatedSeconds)}</span> to
                                   generate, based on your last run.</>}
                            </p>
                            {/* Say this up front. The pipeline degrades to a
                                folder of PNG frames without a muxer, and
                                discovering that after the wait is the worst
                                possible moment to learn it. */}
                            {hasFfmpeg === false && (
                              <p className="text-xs text-amber-400 px-1">
                                No ffmpeg found — video will be saved as numbered PNG frames instead
                                of an mp4. Install ffmpeg, or set <code>VISON_FFMPEG</code> to its path.
                              </p>
                            )}
                          </div>
                        )}

                        {/* Output format applies to every task, so it is not an
                            advanced capability - it is always shown. */}
                        <div className="flex bg-[#2c2c2c] p-1.5 rounded-[12px] items-center px-4">
                          <span className="text-gray-400 text-xs w-20">Format</span>
                          <select
                            className="min-w-0 flex-1 bg-transparent border-none text-white text-sm outline-none text-right"
                            value={settings.outputFormat}
                            onChange={e => setSettings({...settings, outputFormat: e.target.value})}
                          >
                            <option value="png">PNG</option>
                            <option value="jpg">JPG</option>
                          </select>
                        </div>

                        {/* Advanced: driven entirely by what the selected model
                            declares it supports, so no control here is inert. */}
                        {advancedCaps.length > 0 && (
                          <div className="rounded-[12px] border border-[#343434] bg-[#181818] p-3 space-y-3">
                            <button
                              type="button"
                              onClick={() => setShowAdvanced(!showAdvanced)}
                              className="flex w-full items-center justify-between text-left"
                            >
                              <span className="text-sm font-semibold text-white">Advanced</span>
                              <ChevronDown className={`w-4 h-4 text-gray-500 transition-transform ${showAdvanced ? 'rotate-180' : ''}`} />
                            </button>

                            {showAdvanced && (
                              <div className="space-y-2">
                                <p className="text-xs text-gray-500">
                                  Only the settings {selectedModel.name} actually supports are listed.
                                </p>

                                {(supportsAdvanced('tile_size') || supportsAdvanced('gpu_id')) && (
                                  <div className="grid grid-cols-2 gap-2">
                                    {supportsAdvanced('tile_size') && (
                                      <div className="flex bg-[#2c2c2c] p-1.5 rounded-[12px] items-center px-4">
                                        <span className="text-gray-400 text-xs w-20" title="VAE decode tile edge in pixels. 0 uses the default (256px).">Tile px</span>
                                        <input
                                          type="number"
                                          min="0"
                                          step="64"
                                          placeholder="auto"
                                          className="min-w-0 w-16 flex-1 bg-transparent border-none text-white text-sm outline-none text-right"
                                          value={settings.tileSize}
                                          onChange={e => setSettings({...settings, tileSize: Number(e.target.value)})}
                                        />
                                      </div>
                                    )}
                                    {supportsAdvanced('gpu_id') && (
                                      <div className="flex bg-[#2c2c2c] p-1.5 rounded-[12px] items-center px-4">
                                        <span className="text-gray-400 text-xs w-16" title="Which GPU to run on. Changing this reloads the model.">GPU</span>
                                        {/* Populated from /api/system, whose `index` is the same
                                            number the backend means by "vulkan<N>". Falls back to a
                                            text field only if the device list could not be read. */}
                                        {gpuDevices.length > 0 ? (
                                          <select
                                            className="min-w-0 flex-1 bg-transparent border-none text-white text-sm outline-none text-right"
                                            value={settings.gpuId}
                                            onChange={e => setSettings({...settings, gpuId: e.target.value})}
                                          >
                                            <option value="">Auto</option>
                                            {gpuDevices.map((d: any) => (
                                              <option key={d.index} value={String(d.index)}>
                                                {d.name}{d.integrated ? ' (integrated)' : ''} — {Number(d.vram_gb).toFixed(1)} GB
                                              </option>
                                            ))}
                                          </select>
                                        ) : (
                                          <input
                                            type="text"
                                            className="min-w-0 w-16 flex-1 bg-transparent border-none text-white text-sm outline-none text-right"
                                            value={settings.gpuId}
                                            onChange={e => setSettings({...settings, gpuId: e.target.value})}
                                            placeholder="0"
                                          />
                                        )}
                                      </div>
                                    )}
                                  </div>
                                )}

                                {supportsAdvanced('compression') && (
                                  <div className="flex bg-[#2c2c2c] p-1.5 rounded-[12px] items-center px-4">
                                    {/* JPEG and PNG mean genuinely different things by
                                        "compression", so label it for the chosen format
                                        rather than pretending one scale fits both. */}
                                    <span
                                      className="text-gray-400 text-xs w-24"
                                      title={settings.outputFormat === 'jpg'
                                        ? 'JPEG quality 1-100. 0 uses the default (92).'
                                        : 'PNG is lossless; 1-9 trades encode time against file size. 0 uses the default.'}
                                    >
                                      {settings.outputFormat === 'jpg' ? 'Quality' : 'PNG level'}
                                    </span>
                                    <input
                                      type="number"
                                      min="0"
                                      max={settings.outputFormat === 'jpg' ? 100 : 9}
                                      placeholder="auto"
                                      className="min-w-0 w-16 flex-1 bg-transparent border-none text-white text-sm outline-none text-right"
                                      value={settings.compression}
                                      onChange={e => setSettings({...settings, compression: Number(e.target.value)})}
                                    />
                                  </div>
                                )}

                                {(supportsAdvanced('tta_mode') || supportsAdvanced('allow_fallback')) && (
                                  <div className="flex flex-col gap-2 bg-[#2c2c2c] p-3 rounded-[12px]">
                                    {supportsAdvanced('tta_mode') && (
                                      <label className="flex items-center gap-2 text-xs text-gray-300 cursor-pointer">
                                        <input
                                          type="checkbox"
                                          checked={settings.ttaMode}
                                          onChange={e => setSettings({...settings, ttaMode: e.target.checked})}
                                        />
                                        <span>TTA mode <span className="text-gray-500">— cleaner edges, ~8x slower</span></span>
                                      </label>
                                    )}
                                    {supportsAdvanced('allow_fallback') && (
                                      <label className="flex items-center gap-2 text-xs text-gray-300 cursor-pointer">
                                        <input
                                          type="checkbox"
                                          checked={settings.allowFallback}
                                          onChange={e => setSettings({...settings, allowFallback: e.target.checked})}
                                        />
                                        <span>Allow CPU fallback <span className="text-gray-500">— runs if the GPU can't, far slower</span></span>
                                      </label>
                                    )}
                                  </div>
                                )}
                              </div>
                            )}
                          </div>
                        )}

                        {/* Diagnostics is about network reachability, not the
                            task - it used to be hidden unless you were upscaling. */}
                        <div className="space-y-3 rounded-[12px] border border-[#343434] bg-[#181818] p-4">
                          <div className="flex items-center justify-between gap-3">
                            <div>
                              <h4 className="text-sm font-semibold text-white">Diagnostics</h4>
                              <p className="text-xs text-gray-500">Checks whether Hugging Face endpoints are reachable from this machine.</p>
                            </div>
                            <button
                              type="button"
                              onClick={runConnectivityDiagnostics}
                              disabled={diagnosticLoading}
                              className="inline-flex items-center gap-2 rounded-lg bg-[#2f2f2f] px-3 py-2 text-xs font-medium text-white hover:bg-[#3a3a3a] disabled:opacity-50"
                            >
                              <Wifi className={`w-4 h-4 ${diagnosticLoading ? 'animate-pulse' : ''}`} />
                              {diagnosticLoading ? 'Checking...' : 'Check Network'}
                            </button>
                          </div>
                          {diagnosticResult && (
                            <pre className="max-h-40 overflow-y-auto whitespace-pre-wrap rounded-lg bg-black/40 p-3 text-[11px] leading-4 text-gray-300 border border-[#2f2f2f]">
                              {diagnosticResult}
                            </pre>
                          )}
                        </div>

                    {/* Model Dropdown */}
                    <div className="flex flex-col gap-2">
                       <div className="relative">
                          <input 
                            type="text" 
                            className="w-full bg-[#181818] border border-[#333333] text-sm text-white focus:outline-none placeholder-gray-500 rounded-[12px] px-4 py-3"
                            placeholder="Search & Select model..."
                            value={searchModel}
                            onChange={e => setSearchModel(e.target.value)}
                          />
                          <ChevronDown className="absolute right-4 top-1/2 -translate-y-1/2 w-4 h-4 text-gray-500" />
                       </div>
                       
                       <div className="max-h-48 overflow-y-auto bg-[#181818] rounded-[16px] border border-[#333333]">
                         {filteredModels.map(model => (
                           <div
                             key={model.id}
                             role="button"
                             tabIndex={0}
                             onClick={() => {
                               setSelectedModel({ id: model.id, name: model.name, task: model.task });
                               setShowModelDropdown(false);
                               setSearchModel("");
                             }}
                             className={`w-full flex items-center justify-between px-4 py-3 text-left hover:bg-[#2c2c2c] transition-colors ${selectedModel.id === model.id ? 'bg-[#2c2c2c] text-white' : 'text-gray-300'}`}
                           >
                             <span className="text-sm truncate pr-2 font-medium flex items-center gap-2">
                                <Wand2 className="w-4 h-4 text-yellow-400" /> {model.name}
                             </span>
                               <div className="flex items-center gap-2">
                                 <span className="text-[11px] text-gray-500 font-medium whitespace-nowrap">
                                   {formatSize(model)}
                                 </span>
                                <button
                                  type="button"
                                  title="Reset cache"
                                  className="p-1 rounded text-gray-500 hover:text-white hover:bg-[#404040] transition-colors"
                                  onClick={(e) => handleModelResetCache(e, model.id)}
                                >
                                  <RefreshCcw className="w-4 h-4" />
                                </button>
                                {downloadingModels[model.id] ? (
                                  <div className="flex flex-col items-end gap-1 w-20 relative group">
                                     <div className="flex items-center gap-1">
                                       <button
                                        type="button"
                                        title="Cancel download"
                                        className="absolute left-[-20px] p-2 -my-2 cursor-pointer opacity-0 group-hover:opacity-100 transition-all z-10 flex items-center justify-center"
                                        onClick={(e) => handleModelCancel(e, model.id)}
                                       >
                                        <X className="w-4 h-4 text-red-500 hover:scale-125 transition-transform" />
                                       </button>
                                       <Loader2 className="w-3 h-3 text-blue-500 flex-shrink-0 animate-spin group-hover:opacity-0 transition-opacity" />
                                       <span className="text-[10px] text-blue-400 font-mono w-6 text-right">{downloadProgressStates[model.id] || 0}%</span>
                                     </div>
                                     <div className="w-full h-1 bg-[#181818] rounded-full overflow-hidden">
                                        <div className="bg-blue-500 h-full transition-all" style={{ width: `${downloadProgressStates[model.id] || 0}%` }}></div>
                                     </div>
                                  </div>
                                ) : localModels.includes(model.id) ? (
                                  <button
                                    type="button"
                                    title="Delete downloaded model"
                                    className="p-1 hover:bg-red-500/20 rounded text-green-500 hover:text-red-500 transition-colors group/trash"
                                    onClick={(e) => handleModelDelete(e, model.id)}
                                  >
                                    <CheckCircle2 className="w-4 h-4 block group-hover/trash:hidden" />
                                    <Trash2 className="w-4 h-4 hidden group-hover/trash:block" />
                                  </button>
                                ) : (
                                  <button type="button" title="Download model" className="p-1 hover:bg-[#404040] rounded text-gray-500 hover:text-white transition-colors" onClick={(e) => handleModelDownload(e, model.id)}>
                                    <Download className="w-4 h-4" />
                                  </button>
                                )}
                               </div>
                           </div>
                         ))}
                         {filteredModels.length === 0 && (
                           <div className="px-4 py-4 text-sm text-gray-500 text-center font-medium">No models found for this category</div>
                         )}
                       </div>
                    </div>
                  </div>
                )}

                {/* Generate Button inside Pill Wrapper */}
                <div className="ml-1">
                  {isGenerating ? (
                    <button
                      type="button"
                      onClick={() => { void handleCancelGeneration(); }}
                      className="w-9 h-9 rounded-full transition-colors flex items-center justify-center bg-gray-200 text-gray-900 shadow-md"
                    >
                      <Square className="w-3.5 h-3.5 fill-current" />
                    </button>
                  ) : (
                    <button
                      type="submit"
                      disabled={(taskMode === 'upscale' && !attachedImage) || (taskMode !== 'upscale' && !prompt.trim() && !attachedImage) || !localModels.includes(selectedModel.id)}
                      className={`w-9 h-9 rounded-full transition-colors flex items-center justify-center shadow-md ${(taskMode === 'upscale' && !attachedImage) || (taskMode !== 'upscale' && !prompt.trim() && !attachedImage) || !localModels.includes(selectedModel.id) ? 'bg-[#333333] text-gray-500' : 'bg-gray-200 text-gray-900 hover:bg-white'}`}
                    >
                      <ArrowUp className="w-5 h-5 font-bold" />
                    </button>
                  )}
                </div>

              </div>
            </div>
          </form>
        </div>
      )}

      {/* Settings Modal Layer */}
      {noticesText !== null && (
        <div
          className="fixed inset-0 z-[60] flex items-center justify-center bg-black/70 p-6"
          onClick={() => setNoticesText(null)}
        >
          <div
            className="flex max-h-[80vh] w-full max-w-3xl flex-col rounded-2xl border border-[#343434] bg-[#1c1c1c] shadow-2xl"
            onClick={e => e.stopPropagation()}
          >
            <div className="flex items-center justify-between border-b border-[#2c2c2c] px-5 py-4">
              <h3 className="text-base font-semibold text-white">Third-party licences</h3>
              <X
                className="h-4 w-4 cursor-pointer text-gray-400 hover:text-white"
                onClick={() => setNoticesText(null)}
              />
            </div>
            <pre className="flex-1 overflow-auto whitespace-pre-wrap break-words px-5 py-4 text-xs leading-relaxed text-gray-400">
              {noticesText}
            </pre>
          </div>
        </div>
      )}

      {/* Support Vison.
          Deliberately a plain list of links rather than a payment form: taking
          money inside the app would mean handling card details in an Electron
          renderer, and every platform below already does that properly. The
          app's job is to open the browser at the right page. */}
      {showSupport && (
        <div
          className="fixed inset-0 z-[60] flex items-center justify-center bg-black/70 p-6"
          onClick={() => { setShowSupport(false); setSupportError(null); }}
        >
          <div
            className="flex max-h-[80vh] w-full max-w-lg flex-col rounded-2xl border border-[#343434] bg-[#1c1c1c] shadow-2xl"
            onClick={e => e.stopPropagation()}
          >
            <div className="flex items-center justify-between border-b border-[#2c2c2c] px-5 py-4">
              <h3 className="flex items-center gap-2 text-base font-semibold text-white">
                <Heart className="h-4 w-4 text-red-400" />
                Support Vison
              </h3>
              <X
                className="h-4 w-4 cursor-pointer text-gray-400 hover:text-white"
                onClick={() => { setShowSupport(false); setSupportError(null); }}
              />
            </div>

            <div className="flex-1 space-y-5 overflow-y-auto px-5 py-4">
              <p className="text-sm leading-relaxed text-gray-400">
                Vison is free and MIT licensed, and it stays that way. There is no paid
                tier, nothing held back, and no plan to add either. Everything still runs
                on your own machine — supporting the project does not send anything
                anywhere.
              </p>

              {FUNDING_LINKS.filter(link => link.url).length > 0 ? (
                <div className="space-y-2">
                  {FUNDING_LINKS.filter(link => link.url).map(link => (
                    <button
                      key={link.id}
                      type="button"
                      onClick={() => openSupportLink(link.url)}
                      className="group flex w-full items-center justify-between gap-3 rounded-xl border border-[#343434] bg-[#242424] px-4 py-3 text-left transition-colors hover:border-[#4a4a4a] hover:bg-[#2c2c2c]"
                    >
                      <span className="min-w-0">
                        <span className="block text-sm font-medium text-gray-100">{link.label}</span>
                        <span className="block text-xs leading-relaxed text-gray-500">{link.detail}</span>
                      </span>
                      <ExternalLink className="h-4 w-4 shrink-0 text-gray-500 group-hover:text-gray-300" />
                    </button>
                  ))}
                </div>
              ) : (
                <p className="rounded-xl border border-[#343434] bg-[#242424] px-4 py-3 text-xs text-gray-500">
                  No donation link is set up yet. The ways to help below are worth more
                  anyway.
                </p>
              )}

              <div className="space-y-2">
                <p className="text-xs font-medium uppercase tracking-wide text-gray-500">
                  Or help without paying anything
                </p>
                {CONTRIBUTION_LINKS.filter(link => link.url).map(link => (
                  <button
                    key={link.id}
                    type="button"
                    onClick={() => openSupportLink(link.url)}
                    className="group flex w-full items-center justify-between gap-3 rounded-xl border border-transparent px-4 py-2.5 text-left transition-colors hover:border-[#343434] hover:bg-[#242424]"
                  >
                    <span className="min-w-0">
                      <span className="block text-sm text-gray-300">{link.label}</span>
                      <span className="block text-xs leading-relaxed text-gray-500">{link.detail}</span>
                    </span>
                    <ExternalLink className="h-4 w-4 shrink-0 text-gray-600 group-hover:text-gray-400" />
                  </button>
                ))}
              </div>

              <p className="border-t border-[#2c2c2c] pt-4 text-xs leading-relaxed text-gray-500">
                Vison is one developer on one machine. Money goes to hardware it has
                never been tested on — three of the registered models do not fit on the
                6 GB laptop GPU it was built against — and to a code signing
                certificate, so the installer stops tripping SmartScreen.
              </p>

              {supportError && (
                <p className="text-xs text-red-400">{supportError}</p>
              )}
            </div>
          </div>
        </div>
      )}

      {showSettings && currentView === 'chat' && !selectedModel.task.includes("upscale") && (
        <div className="absolute top-16 left-6 bg-[#242424] border border-[#343434] p-5 rounded-2xl shadow-2xl z-50 w-80 text-sm">
           {/* Setting Options remain the same, just truncated here to save space as I've fully rewritten the parent scope... Let's re-add them below so it doesn't break */}
          <div className="flex justify-between items-center mb-4 text-white">
             <h3 className="font-semibold text-base">Generation Settings</h3>
             <X className="w-4 h-4 cursor-pointer hover:text-gray-300" onClick={() => setShowSettings(false)} />
          </div>
          <div className="space-y-4">
             <div>
                <label className="block text-gray-400 mb-1">Negative Prompt</label>
                <input type="text" className="w-full bg-[#181818] border border-[#343434] rounded-lg px-3 py-2 text-white outline-none" value={settings.negativePrompt} onChange={e => setSettings({...settings, negativePrompt: e.target.value})} />
             </div>
             {selectedModel.task.includes("upscale") ? (
                 <div>
                   <label className="block text-gray-400 mb-1">Quality / Upscale Factor</label>
                   <select 
                     className="w-full bg-[#181818] border border-[#343434] rounded-lg px-3 py-2 text-white outline-none"
                     value={settings.upscaleQuality} 
                     onChange={e => setSettings({...settings, upscaleQuality: e.target.value})}
                   >
                     <option value="2x">2x</option>
                     <option value="4x">4x</option>
                     <option value="1080p">1080p</option>
                     <option value="1440p">1440p</option>
                     <option value="2160p">2160p / 4K</option>
                   </select>
                 </div>
               ) : (
                 <div className="flex gap-3">
                    <div className="flex-1">
                       <label className="block text-gray-400 mb-1">Width</label>
                       <input type="number" step="64" className="w-full bg-[#181818] border border-[#343434] rounded-lg px-3 py-2 text-white outline-none" value={settings.width} onChange={e => setSettings({...settings, width: Number(e.target.value)})} />
                    </div>
                    <div className="flex-1">
                       <label className="block text-gray-400 mb-1">Height</label>
                       <input type="number" step="64" className="w-full bg-[#181818] border border-[#343434] rounded-lg px-3 py-2 text-white outline-none" value={settings.height} onChange={e => setSettings({...settings, height: Number(e.target.value)})} />
                    </div>
                 </div>
               )}
             <div className="flex gap-3">
                <div className="flex-1">
                   <label className="block text-gray-400 mb-1">Steps</label>
                   <input type="number" className="w-full bg-[#181818] border border-[#343434] rounded-lg px-3 py-2 text-white outline-none" value={settings.steps} onChange={e => setSettings({...settings, steps: Number(e.target.value)})} />
                </div>
                <div className="flex-1">
                   <label className="block text-gray-400 mb-1">Guidance</label>
                   <input type="number" step="0.5" className="w-full bg-[#181818] border border-[#343434] rounded-lg px-3 py-2 text-white outline-none" value={settings.guidance} onChange={e => setSettings({...settings, guidance: Number(e.target.value)})} />
                </div>
             </div>
          </div>
        </div>
      )}
    </div>
  );
}

export default App;
