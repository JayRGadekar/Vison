import os
import glob
import shutil
import threading
import time
import subprocess
# Prefer the mirror by default because this environment reliably resets Hugging Face primary connections.
os.environ.setdefault("HF_ENDPOINT", "https://hf-mirror.com")
os.environ.setdefault("HF_HUB_ENABLE_HF_TRANSFER", "1")

download_cancel_flags = {}

import asyncio
generation_lock = asyncio.Lock()
import huggingface_hub.utils
from tqdm import tqdm as std_tqdm

class VisonTqdm(std_tqdm):
    current_model = "unknown"
    
    def __init__(self, *args, **kwargs):
        kwargs.pop("name", None)
        super().__init__(*args, **kwargs)

    def display(self, msg=None, pos=None):
        if download_cancel_flags.get(VisonTqdm.current_model):
            raise Exception(f"CancelledByUser")
        super().display(msg, pos)
        try:
            total = getattr(self, "total", 0)
            n = getattr(self, "n", 0)
            if total and type(total) in (int, float) and total > 0:
                pct = round((n / total) * 100)
                # print(f"TQDM UPDATE: {pct}%", flush=True) # noisy
                if "ws_clients" in globals() and "app_loop" in globals() and globals()["app_loop"]:
                    for c in globals()["ws_clients"]:
                        asyncio.run_coroutine_threadsafe(
                            c.send_json({"type": "download_progress", "model": VisonTqdm.current_model, "progress": pct}),
                            globals()["app_loop"]
                        )
        except Exception as e:
            pass
            
    def update(self, n=1):
        if download_cancel_flags.get(VisonTqdm.current_model):
            raise Exception(f"CancelledByUser")
        super().update(n)
        try:
            total = getattr(self, "total", 0)
            n_val = getattr(self, "n", 0)
            if total and type(total) in (int, float) and total > 0:
                pct = round((n_val / total) * 100)
                if "ws_clients" in globals() and "app_loop" in globals() and globals()["app_loop"]:
                    for c in globals()["ws_clients"]:
                        asyncio.run_coroutine_threadsafe(
                            c.send_json({"type": "download_progress", "model": VisonTqdm.current_model, "progress": pct}),
                            globals()["app_loop"]
                        )
        except Exception as e:
            pass

import huggingface_hub.utils.tqdm
huggingface_hub.utils.tqdm.tqdm = VisonTqdm
# Also try patching it on snapshot_download explicitly via kwargs when calling it

from fastapi import FastAPI, BackgroundTasks, WebSocket, WebSocketDisconnect
from fastapi.middleware.cors import CORSMiddleware
from fastapi.staticfiles import StaticFiles
from starlette.concurrency import run_in_threadpool
from pydantic import BaseModel
from huggingface_hub import snapshot_download, scan_cache_dir
import uvicorn
import os
import signal
import sys
import asyncio
import base64
from io import BytesIO
from PIL import Image
from contextlib import asynccontextmanager
import gc
import torch
import requests



from models.image import CACHE_DIR, ImageGenerator

import contextlib

@asynccontextmanager
async def lifespan(app: FastAPI):
    global app_loop
    app_loop = asyncio.get_running_loop()
    yield

app = FastAPI(title="Vison API", lifespan=lifespan)

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

os.makedirs(os.path.join(CACHE_DIR, "outputs"), exist_ok=True)
app.mount("/outputs", StaticFiles(directory=os.path.join(CACHE_DIR, "outputs")), name="outputs")

active_generator = None
app_loop = None
ws_clients = []
downloading_models = {}
download_activity = {}
generation_cancel_requested = False
DOWNLOAD_STALL_TIMEOUT_SECONDS = 30
DOWNLOAD_RETRY_ATTEMPTS_PER_ENDPOINT = 4
DOWNLOAD_RETRY_BASE_DELAY_SECONDS = 1.5
DOWNLOAD_RETRY_MAX_DELAY_SECONDS = 20.0

# Backward-compatible aliases for old model IDs that may still exist in UI state.
MODEL_ID_ALIASES = {
    "xinntao/Real-ESRGAN": "Comfy-Org/Real-ESRGAN_repackaged",
    "Bxb100/RealESRGAN_x4plus": "Comfy-Org/Real-ESRGAN_repackaged",
}


def _repo_cache_dir_for(repo_id: str) -> str:
    return os.path.join(CACHE_DIR, f"models--{repo_id.replace('/', '--')}")


def _latest_snapshot_dir(repo_id: str) -> str | None:
    snapshots_dir = os.path.join(_repo_cache_dir_for(repo_id), "snapshots")
    if not os.path.isdir(snapshots_dir):
        return None
    candidates = []
    for entry in os.listdir(snapshots_dir):
        full = os.path.join(snapshots_dir, entry)
        if os.path.isdir(full):
            candidates.append(full)
    if not candidates:
        return None
    candidates.sort(key=lambda p: os.path.getmtime(p), reverse=True)
    return candidates[0]


def _cli_snapshot_download(
    repo_id: str,
    cache_dir: str,
    endpoint: str,
    token: str | None,
    max_workers: int = 1,
    force_download: bool = False,
) -> str:
    env = os.environ.copy()
    env["HF_ENDPOINT"] = endpoint
    env["HF_HUB_ENABLE_HF_TRANSFER"] = "1"
    if token:
        env["HF_TOKEN"] = token

    cmd_variants = [
        [
            sys.executable,
            "-m",
            "huggingface_hub.commands.huggingface_cli",
            "download",
            repo_id,
            "--cache-dir",
            cache_dir,
            "--max-workers",
            str(max_workers),
        ],
        [
            "huggingface-cli",
            "download",
            repo_id,
            "--cache-dir",
            cache_dir,
            "--max-workers",
            str(max_workers),
        ],
    ]

    if force_download:
        for cmd in cmd_variants:
            cmd.append("--force-download")

    last_err: Exception | None = None
    for cmd in cmd_variants:
        try:
            proc = subprocess.run(cmd, capture_output=True, text=True, env=env, timeout=60 * 60)
        except FileNotFoundError as e:
            last_err = e
            continue

        if proc.returncode != 0:
            msg = (proc.stderr or proc.stdout or "").strip()
            last_err = RuntimeError(f"CLI download failed for {repo_id}: {msg}")
            continue

        text = (proc.stdout or "") + "\n" + (proc.stderr or "")
        for line in reversed([ln.strip() for ln in text.splitlines() if ln.strip()]):
            if os.path.isdir(line):
                return line

        latest = _latest_snapshot_dir(repo_id)
        if latest and os.path.isdir(latest):
            return latest

    raise last_err if last_err is not None else RuntimeError(f"CLI download failed for {repo_id}")


def _snapshot_is_usable(snapshot_path: str) -> bool:
    if not snapshot_path or not os.path.isdir(snapshot_path):
        return False

    has_model_index = os.path.isfile(os.path.join(snapshot_path, "model_index.json"))
    has_component_config = bool(glob.glob(os.path.join(snapshot_path, "**", "config.json"), recursive=True))
    has_weights = bool(glob.glob(os.path.join(snapshot_path, "**", "*.safetensors"), recursive=True)) or bool(
        glob.glob(os.path.join(snapshot_path, "**", "*.bin"), recursive=True)
    ) or bool(glob.glob(os.path.join(snapshot_path, "**", "*.pth"), recursive=True))

    # Diffusers-style repos need index + configs + weights.
    if has_model_index:
        return has_component_config and has_weights

    # Non-diffusers checkpoints (e.g., ESRGAN) can be considered usable with weights only.
    return has_weights


def _repo_has_usable_snapshot(repo_path: str) -> bool:
    snapshots_dir = os.path.join(repo_path, "snapshots")
    if not os.path.isdir(snapshots_dir):
        return False
    for snap in os.listdir(snapshots_dir):
        snap_path = os.path.join(snapshots_dir, snap)
        if _snapshot_is_usable(snap_path):
            return True
    return False


def _send_download_progress(model_name: str, progress: int) -> None:
    download_activity[model_name] = time.time()
    if "app_loop" in globals() and globals().get("app_loop") and "ws_clients" in globals() and ws_clients:
        for c in ws_clients:
            asyncio.run_coroutine_threadsafe(
                c.send_json({"type": "download_progress", "model": model_name, "progress": progress}),
                globals()["app_loop"]
            )


def _send_download_status(model_name: str, status_text: str, error_message: str | None = None) -> None:
    if "app_loop" in globals() and app_loop and "ws_clients" in dict(globals()) and ws_clients:
        for c in ws_clients:
            asyncio.run_coroutine_threadsafe(
                c.send_json({
                    "type": "download_status",
                    "status": status_text,
                    "model": model_name,
                    "error": error_message,
                }),
                app_loop
            )


def _remove_model_cache(model_id: str) -> bool:
    repo_cache_dir = _repo_cache_dir_for(model_id)
    removed = False
    if os.path.isdir(repo_cache_dir):
        shutil.rmtree(repo_cache_dir, ignore_errors=True)
        removed = True
    return removed


def _env_flag(name: str, default: bool = False) -> bool:
    value = os.environ.get(name)
    if value is None:
        return default
    return value.strip().lower() in {"1", "true", "yes", "on"}


def _download_endpoint_candidates() -> list[str]:
    configured_endpoint = os.environ.get("HF_ENDPOINT", "https://hf-mirror.com")
    official_endpoint = "https://huggingface.co"
    mirror_endpoint = "https://hf-mirror.com"

    # Default to mirror-only routing for downloads on networks where
    # huggingface.co is blocked/reset; enable primary fallback explicitly.
    ordered = [configured_endpoint, mirror_endpoint]
    if _env_flag("VISON_ALLOW_OFFICIAL_HF_FALLBACK", default=False):
        ordered.append(official_endpoint)

    deduped = []
    for endpoint in ordered:
        if endpoint and endpoint not in deduped:
            deduped.append(endpoint)
    if configured_endpoint == official_endpoint and mirror_endpoint in deduped:
        deduped.remove(mirror_endpoint)
        deduped.insert(0, mirror_endpoint)
    return deduped

@app.websocket("/api/ws/progress")
async def ws_progress(ws: WebSocket):
    await ws.accept()
    ws_clients.append(ws)
    try:
        while True:
            await ws.receive_text()
    except WebSocketDisconnect:
        ws_clients.remove(ws)

def notify_progress(step: int, total: int):
    if app_loop and ws_clients:
        for c in ws_clients:
            asyncio.run_coroutine_threadsafe(
                c.send_json({"step": step, "total": total}), 
                app_loop
            )

class GenerateRequest(BaseModel):
    prompt: str
    model: str
    task: str = "image"
    base_image: str | None = None
    negative_prompt: str = ""
    width: int = 512
    height: int = 512
    num_inference_steps: int = 20
    guidance_scale: float = 7.5
    seed: int = -1
    upscale_quality: str = '4x'
    tile_size: int = 0
    tta_mode: bool = False
    compression: int = 0
    output_format: str = 'png'
    gpu_id: str = ''
    allow_fallback: bool = False

class DownloadRequest(BaseModel):
    model: str


@app.post("/api/generate/cancel")
async def cancel_generation():
    global generation_cancel_requested
    generation_cancel_requested = True
    return {"status": "cancelling"}

@app.get("/api/download/status")
async def get_download_status():
    return downloading_models

@app.post("/api/download/cancel")
async def cancel_download_model(req: DownloadRequest):
    download_cancel_flags[req.model] = True
    downloading_models[req.model] = False
    download_activity.pop(req.model, None)
    return {"status": "cancelled"}

@app.post("/api/download")
async def download_model(req: DownloadRequest):
    client_model_id = req.model
    resolved_model_id = MODEL_ID_ALIASES.get(client_model_id, client_model_id)

    if client_model_id in downloading_models and downloading_models[client_model_id]:
        return {"status": "already_downloading"}
        
    downloading_models[client_model_id] = True
    download_cancel_flags[client_model_id] = False
    download_activity[client_model_id] = time.time()

    def stall_watchdog(model_name):
        while downloading_models.get(model_name):
            last_seen = download_activity.get(model_name, time.time())
            if time.time() - last_seen > DOWNLOAD_STALL_TIMEOUT_SECONDS:
                downloading_models[model_name] = False
                download_cancel_flags[model_name] = True
                _send_download_status(
                    model_name,
                    "failed",
                    f"Download stalled for over {DOWNLOAD_STALL_TIMEOUT_SECONDS}s. Check network/HF endpoint and retry.",
                )
                break
            time.sleep(2)

    def do_download(download_model_name, response_model_name):
        print("INSIDE DO_DOWNLOAD", flush=True)
        success = False
        error_message = None
        try:
            # Keep progress keyed to the model id used by the UI.
            VisonTqdm.current_model = response_model_name
            hf_token = os.environ.get("HF_TOKEN")
            _send_download_progress(response_model_name, 1)

            # Try configured endpoint first, then fallback to mirror candidates.
            deduped_endpoints = _download_endpoint_candidates()

            def _snapshot_download_with_retry(endpoint: str, force_download: bool = False):
                last_exc = None
                for attempt in range(1, DOWNLOAD_RETRY_ATTEMPTS_PER_ENDPOINT + 1):
                    if download_cancel_flags.get(response_model_name):
                        raise Exception("CancelledByUser")
                    try:
                        os.environ["HF_ENDPOINT"] = endpoint
                        return snapshot_download(
                            repo_id=download_model_name,
                            cache_dir=CACHE_DIR,
                            token=hf_token,
                            max_workers=1,
                            force_download=force_download,
                            ignore_patterns=["*.msgpack", "*.safetensors.index.json"]
                        )
                    except Exception as exc:
                        last_exc = exc
                        if "CancelledByUser" in str(exc):
                            raise
                        if attempt < DOWNLOAD_RETRY_ATTEMPTS_PER_ENDPOINT:
                            delay = min(
                                DOWNLOAD_RETRY_BASE_DELAY_SECONDS * (2 ** (attempt - 1)),
                                DOWNLOAD_RETRY_MAX_DELAY_SECONDS,
                            )
                            print(
                                f"Download retry {attempt}/{DOWNLOAD_RETRY_ATTEMPTS_PER_ENDPOINT - 1} via {endpoint} in {delay:.1f}s: {exc}",
                                flush=True,
                            )
                            time.sleep(delay)
                raise last_exc

            last_exc = None
            for ep in deduped_endpoints:
                try:
                    model_path = _snapshot_download_with_retry(ep, force_download=False)

                    # If we somehow got a partial snapshot, purge and re-download once.
                    if not _snapshot_is_usable(model_path):
                        repo_cache = _repo_cache_dir_for(download_model_name)
                        if os.path.isdir(repo_cache):
                            shutil.rmtree(repo_cache, ignore_errors=True)
                        model_path = _snapshot_download_with_retry(ep, force_download=True)
                        if not _snapshot_is_usable(model_path):
                            raise RuntimeError("Download completed but required model files are missing (incomplete snapshot)")

                    last_exc = None
                    break
                except Exception as endpoint_exc:
                    last_exc = endpoint_exc
                    print(f"Download attempt failed via {ep}: {endpoint_exc}", flush=True)
                    try:
                        print(f"Trying CLI fallback via {ep}...", flush=True)
                        model_path = _cli_snapshot_download(
                            repo_id=download_model_name,
                            cache_dir=CACHE_DIR,
                            endpoint=ep,
                            token=hf_token,
                            max_workers=1,
                            force_download=False,
                        )

                        if not _snapshot_is_usable(model_path):
                            repo_cache = _repo_cache_dir_for(download_model_name)
                            if os.path.isdir(repo_cache):
                                shutil.rmtree(repo_cache, ignore_errors=True)
                            model_path = _cli_snapshot_download(
                                repo_id=download_model_name,
                                cache_dir=CACHE_DIR,
                                endpoint=ep,
                                token=hf_token,
                                max_workers=1,
                                force_download=True,
                            )
                            if not _snapshot_is_usable(model_path):
                                raise RuntimeError("CLI fallback completed but required model files are missing (incomplete snapshot)")

                        last_exc = None
                        break
                    except Exception as cli_exc:
                        last_exc = cli_exc
                        print(f"CLI fallback failed via {ep}: {cli_exc}", flush=True)

            if last_exc is not None:
                raise last_exc
            success = True
            
        except Exception as e:
            if "CancelledByUser" in str(e):
                print(f"Download of {download_model_name} cancelled by user.")
                success = "cancelled"
            else:
                error_message = str(e)
                print("ERR downloading model:", e, flush=True)
                success = False
            
        finally:
            downloading_models[response_model_name] = False
            download_activity.pop(response_model_name, None)
            status_text = "completed" if success is True else ("cancelled" if success == "cancelled" else "failed")
            _send_download_status(response_model_name, status_text, error_message)

    try:
        threading.Thread(target=stall_watchdog, args=(client_model_id,), daemon=True).start()
        loop = asyncio.get_running_loop()
        loop.run_in_executor(None, do_download, resolved_model_id, client_model_id)
        return {"status": "started"}
    except Exception as e:
        downloading_models[client_model_id] = False
        return {"status": "failed", "error": str(e)}

@app.post("/api/generate")
async def generate_image(req: GenerateRequest):
    global active_generator, generation_cancel_requested
    init_image = None
    if req.base_image and req.base_image.startswith('data:image'):
        img_str = req.base_image.split(',')[1]
        init_image = Image.open(BytesIO(base64.b64decode(img_str))).convert("RGB")
    
    async with generation_lock:
        generation_cancel_requested = False
        if active_generator is None or active_generator.model_id != req.model:
            # Prevent VRAM Out-of-Memory crashes when switching between massive models
            if active_generator is not None:
                print(f"Unloading previous model {active_generator.model_id} to free VRAM...")
                active_generator.pipeline = None
                active_generator = None
                gc.collect() # Force garbage collection
                import torch
                if torch.cuda.is_available():
                    torch.cuda.empty_cache() # Flush PyTorch CUDA graph

            from models.image import ImageGenerator
            active_generator = ImageGenerator(model_id=req.model)

        def notify_progress(step, totals):
            global app_loop, ws_clients, generation_cancel_requested
            if generation_cancel_requested:
                raise RuntimeError("CancelledByUser")
            if app_loop and ws_clients:
                for c in ws_clients:
                    asyncio.run_coroutine_threadsafe(
                        c.send_json({"type": "progress", "step": step, "total": totals}),
                        app_loop
                    )

        # Run Generation Process in a thread avoiding the event loop block
        def run_gen():
            return active_generator.generate(
                prompt=req.prompt,
                negative_prompt=req.negative_prompt,
                width=req.width,
                height=req.height,
                steps=req.num_inference_steps,
                guidance=req.guidance_scale,
                seed=req.seed,
                upscale_quality=req.upscale_quality,
                tile_size=req.tile_size,
                tta_mode=req.tta_mode,
                compression=req.compression,
                output_format=req.output_format,
                gpu_id=req.gpu_id,
                allow_fallback=req.allow_fallback,
                init_image=init_image,
                progress_cb=notify_progress
            )
        try:
            img_path = await run_in_threadpool(run_gen)
            output_name = os.path.basename(img_path)
            return {"status": "success", "image_url": f"http://127.0.0.1:11439/outputs/{output_name}"}
        except Exception as e:
            if "CancelledByUser" in str(e):
                return {"status": "cancelled", "message": "Generation cancelled by user."}
            print(f"Exception during generation: {e}")
            return {"status": "error", "message": str(e)}
    os.kill(os.getpid(), signal.SIGINT if sys.platform != 'win32' else signal.SIGTERM)
    return {"status": "shutting down"}

@app.get("/api/models/local")
async def get_local_models():
    try:
        cache = scan_cache_dir(CACHE_DIR)
        usable = [repo.repo_id for repo in cache.repos if _repo_has_usable_snapshot(repo.repo_path)]
        return {"downloaded": usable}
    except Exception:
        return {"downloaded": []}

@app.delete("/api/models/{model_id:path}")
async def delete_local_model(model_id: str):
    try:
        cache = scan_cache_dir(CACHE_DIR)
        for repo in cache.repos:
            if repo.repo_id == model_id:
                shutil.rmtree(repo.repo_path)
                return {"status": "deleted"}
        return {"status": "not_found"}
    except Exception as e:
        return {"status": "error", "message": str(e)}


@app.post("/api/models/{model_id:path}/reset-cache")
async def reset_model_cache(model_id: str):
    try:
        download_cancel_flags[model_id] = True
        downloading_models[model_id] = False
        download_activity.pop(model_id, None)

        removed = _remove_model_cache(model_id)
        return {"status": "reset", "removed": removed}
    except Exception as e:
        return {"status": "error", "message": str(e)}


@app.get("/api/diagnostics/connectivity")
async def connectivity_diagnostics():
    probes = [
        ("huggingface_api", "https://huggingface.co/api/models/stabilityai/sdxl-turbo"),
        ("huggingface_readme", "https://huggingface.co/stabilityai/sdxl-turbo/resolve/main/README.md"),
        ("mirror_api", "https://hf-mirror.com/api/models/stabilityai/sdxl-turbo"),
        ("mirror_readme", "https://hf-mirror.com/stabilityai/sdxl-turbo/resolve/main/README.md"),
    ]

    results = []
    for name, url in probes:
        try:
            response = requests.get(url, timeout=12, stream=True)
            chunk = None
            try:
                chunk = next(response.iter_content(chunk_size=1), None)
            except Exception:
                chunk = None
            results.append({
                "name": name,
                "url": url,
                "ok": response.ok,
                "status": response.status_code,
                "got_byte": chunk is not None,
            })
        except Exception as e:
            results.append({
                "name": name,
                "url": url,
                "ok": False,
                "error": str(e),
            })

    result_map = {row["name"]: row for row in results}
    hf_api_ok = bool(result_map.get("huggingface_api", {}).get("ok"))
    hf_readme_ok = bool(result_map.get("huggingface_readme", {}).get("ok"))
    mirror_api_ok = bool(result_map.get("mirror_api", {}).get("ok"))
    mirror_readme_ok = bool(result_map.get("mirror_readme", {}).get("ok"))

    if not hf_api_ok and not hf_readme_ok and mirror_api_ok and mirror_readme_ok:
        likely_cause = "primary huggingface.co is blocked/reset on this network; mirror endpoint is healthy"
    elif (hf_api_ok or mirror_api_ok) and not (hf_readme_ok or mirror_readme_ok):
        likely_cause = "API endpoints are reachable, but file transfer/download traffic is being blocked or reset"
    elif hf_api_ok or hf_readme_ok or mirror_api_ok or mirror_readme_ok:
        likely_cause = "general connectivity looks mostly OK; intermittent resets may still happen during large downloads"
    else:
        likely_cause = "network access to both primary and mirror Hugging Face endpoints is failing"

    return {
        "results": results,
        "summary": likely_cause,
    }

@app.get("/api/models")
async def list_models():
    return {
        "image": [
            {"id": "stabilityai/sdxl-turbo", "name": "SDXL Turbo (Fast)", "size": "6.9 GB", "description": "Blazing fast high-res generation"},
            {"id": "stabilityai/stable-diffusion-xl-base-1.0", "name": "Stable Diffusion XL", "size": "6.9 GB", "description": "The golden standard for text-to-image"},
            {"id": "black-forest-labs/FLUX.1-schnell", "name": "FLUX Schnell", "size": "23.8 GB", "description": "Next-gen ultra-realistic images"},
            {"id": "runwayml/stable-diffusion-v1-5", "name": "Stable Diffusion 1.5", "size": "4.3 GB", "description": "Classic, highly compatible base model"},
            {"id": "prompthero/openjourney", "name": "OpenJourney", "size": "4.3 GB", "description": "Midjourney v4 style model"},
            {"id": "Lykon/dreamshaper-xl-1-0", "name": "Dreamshaper XL", "size": "6.9 GB", "description": "Highly stylized artistic models"},
        ],
        "video": [
            {"id": "stabilityai/stable-video-diffusion-img2vid-xt", "name": "Stable Video Diffusion XT", "size": "9.5 GB", "description": "High quality Image-to-Video generation"},
            {"id": "THUDM/CogVideoX-2b", "name": "CogVideoX 2B", "size": "14.5 GB", "description": "Next generation text-to-video"},
            {"id": "THUDM/CogVideoX-5b", "name": "CogVideoX 5B", "size": "30.0 GB", "description": "Flagship cinematic text-to-video"},
            {"id": "ali-vilab/text-to-video-ms-1.7b", "name": "ModelScope Damo", "size": "6.5 GB", "description": "Fast open-source text-to-video"},
            {"id": "lucataco/animate-diff", "name": "AnimateDiff", "size": "8.0 GB", "description": "Classic animation library"},
        ],
        "image_upscale": [
            {"id": "stabilityai/stable-diffusion-x4-upscaler", "name": "SD X4 Upscaler", "size": "4.5 GB", "description": "Latent upscale model"},
              {"id": "Comfy-Org/Real-ESRGAN_repackaged", "name": "Real-ESRGAN x4", "size": "0.1 GB", "description": "Extremely fast algorithmic upscaler"},
              {"id": "caidas/swin2SR-classical-sr-x2-64", "name": "Swin2SR Upscaler", "size": "0.2 GB", "description": "Transformer based upscaler"},
              {"id": "kandinsky-community/kandinsky-2-2-decoder-inpaint", "name": "Kandinsky Inpaint/Upscale", "size": "5.0 GB", "description": "Versatile upscale pipeline"},
              {"id": "Saiki/Real-ESRGAN-ANIME", "name": "RealESRGAN+ Anime", "size": "0.1 GB", "description": "Optimized for stylized/anime art"}
        ],
        "video_upscale": [
            {"id": "kadirnar/video-retasking", "name": "Video Upscaler Basic", "size": "2.1 GB", "description": "Basic video upscaler"},
        ]
    }

if __name__ == "__main__":
    # Electron already handles process restarts; reload mode causes duplicate processes and port conflicts.
    uvicorn.run("main:app", host="127.0.0.1", port=11439, reload=False)
