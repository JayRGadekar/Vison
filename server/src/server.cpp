#include "httplib.h"
#include "nlohmann/json.hpp"
#include "vison/vison.h"
#include "vison/queue.h"
#include "vison/download.h"
#include "vison/pipelines/video_io.h"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <future>
#include <mutex>
#include <atomic>
#include <thread>
#include <set>
#include <vector>
#include <chrono>
#include <map>
#include <algorithm>
#include <exception>
#include <cstdlib>
#include <cstdio>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

using json = nlohmann::json;

struct DownloadState {
    std::mutex mtx;
    std::string model_id;
    std::string status = "idle";
    float progress = 0.0f;
    std::string current_file;   // which component is transferring right now
    int file_index = 0;         // 1-based, for "file 2 of 4"
    int file_count = 0;
    std::string error_message;
    std::atomic<bool> cancel{false};
};
DownloadState g_download;

// Where models and outputs live.
//
// These used to be bare relative paths, which works when the server is started
// from the repo but breaks the moment it is installed: a packaged app runs from
// somewhere like C:\Program Files\Vison, and writing a 7GB download there
// fails (read-only for a normal user) or lands somewhere surprising. The
// launcher passes VISON_DATA_DIR pointing at per-user storage; falling back to
// the working directory keeps the development workflow exactly as it was.
std::filesystem::path g_data_dir = [] {
    if (const char* env = std::getenv("VISON_DATA_DIR")) {
        if (*env) {
            std::error_code ec;
            std::filesystem::create_directories(env, ec);
            return std::filesystem::path(env);
        }
    }
    return std::filesystem::current_path();
}();

// Absolute path for something inside the data directory.
std::string data_path(const std::string& relative) {
    return (g_data_dir / relative).string();
}

std::string g_models_dir = data_path("models");
std::mutex g_config_mutex;

// --- WebSocket progress fan-out ---------------------------------------------
// Connected /api/ws/progress clients. Generation runs on the queue worker
// thread, so every send is serialised through g_ws_mutex.
std::mutex g_ws_mutex;
std::vector<httplib::ws::WebSocket*> g_ws_clients;

void broadcast_ws(const json& message) {
    const std::string payload = message.dump();
    std::lock_guard<std::mutex> lock(g_ws_mutex);
    for (auto* ws : g_ws_clients) {
        if (ws && ws->is_open()) {
            // A client that vanished mid-send just fails here; the handler
            // thread for that socket removes it from the list on its way out.
            ws->send(payload);
        }
    }
}

// Monotonic id for output filenames, so two generations finishing close
// together cannot overwrite one another.
std::atomic<uint64_t> g_output_counter{0};

uint64_t next_output_id() {
    auto now = std::chrono::system_clock::now().time_since_epoch();
    auto ms = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    return ms * 1000 + (g_output_counter++ % 1000);
}

// Minimal RFC 4648 base64 decoder for data: URLs from the UI.
bool base64_decode(const std::string& input, std::string& out) {
    auto value_of = [](unsigned char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };

    out.clear();
    out.reserve(input.size() * 3 / 4);

    int buffer = 0;
    int bits = 0;
    for (unsigned char c : input) {
        if (c == '=' ) break;
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
        int v = value_of(c);
        if (v < 0) return false;
        buffer = (buffer << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((char)((buffer >> bits) & 0xFF));
        }
    }
    return true;
}



// Model registry.
//
// Every model declares the COMPLETE set of files it needs to run. This matters
// for FLUX: the diffusion transformer alone is useless, it also needs the two
// text encoders (CLIP-L, T5-XXL) and the VAE. Downloading only the .gguf
// produced a model that passed the "file exists" check and then failed at load.
//
// "role" tells the pipeline what each file is:
//   diffusion - the main .gguf passed as model_path (exactly one per model)
//   clip_l / t5xxl / vae - companion weights loaded alongside it
//
// size_bytes is the exact Content-Length used to verify a completed download.
// 0 means "unknown", which skips the size check but still enforces format magic.
json get_default_registry() {
    return json::parse(R"JSON({
      "version": 2,
      "models": {
        "image_generation": [
          {
            "id": "stabilityai/sdxl-turbo",
            "description": "Fast, few-step image generation at 512-1024px. The lightest option and the only single-file one: no separate text encoder to load, so it starts quickly and asks least of a small machine.",
            "name": "SDXL Turbo (Lightest)",
            "advanced": ["gpu_id", "allow_fallback", "tile_size", "compression"],
            "default_width": 768,
            "default_height": 768,
            "default_steps": 4,
            "default_guidance": 1.0,
            "_defaults_comment": "SDXL Turbo is adversarially distilled for 1-4 steps at cfg 1.0. Running it at the generic cfg 7.5 over 20 steps burns five times the compute to produce a worse, over-saturated image.",
            "size_gb": 3.82,
            "vram_min_gb": 4,
            "quantizations": ["q4_0", "q5_0", "q8_0", "f16"],
            "files": [
              {
                "role": "diffusion",
                "filename": "stabilityai_sdxl-turbo.gguf",
                "url": "https://huggingface.co/OlegSkutte/sdxl-turbo-GGUF/resolve/main/sd_xl_turbo_1.0.q8_0.gguf",
                "size_bytes": 4098988672
              }
            ]
          },
          {
            "id": "tongyi-milm/z-image-turbo",
            "description": "A far newer design than SDXL at a similar footprint: it reads the prompt with a 4B language model instead of CLIP, so it follows long, specific instructions much more closely. Eight steps, and it runs on a 4 GB card.",
            "name": "Z-Image Turbo (Balanced)",
            "advanced": ["gpu_id", "allow_fallback", "tile_size", "compression"],
            "default_width": 1024,
            "default_height": 1024,
            "default_steps": 8,
            "default_guidance": 1.0,
            "_defaults_comment": "Distilled for 8 steps at cfg 1.0, per stable-diffusion.cpp's own z_image.md. Raising cfg on a distilled model does not sharpen it, it burns it.",
            "_comment": "6B DiT from Alibaba Tongyi, conditioned on Qwen3-4B rather than CLIP/T5 - which is why it needs the llm role. Shares ae.safetensors with FLUX.1 Schnell (the same Flux VAE, byte-identical), so whichever of the two is downloaded second gets that file for free. Weights are leejet's own GGUF conversion, i.e. quantised by the author of the inference engine we vendor. REGISTERED BUT NEVER RUN HERE.",
            "size_gb": 7.54,
            "vram_min_gb": 4,
            "files": [
              {
                "role": "diffusion",
                "filename": "z_image_turbo-Q6_K.gguf",
                "url": "https://huggingface.co/leejet/Z-Image-Turbo-GGUF/resolve/main/z_image_turbo-Q6_K.gguf",
                "size_bytes": 5263239104
              },
              {
                "role": "llm",
                "_comment": "Text conditioning. Z-Image has no CLIP or T5 at all.",
                "filename": "Qwen3-4B-Instruct-2507-Q4_K_M.gguf",
                "url": "https://huggingface.co/unsloth/Qwen3-4B-Instruct-2507-GGUF/resolve/main/Qwen3-4B-Instruct-2507-Q4_K_M.gguf",
                "size_bytes": 2497281120
              },
              {
                "role": "vae",
                "_comment": "The Flux VAE - Z-Image uses it unchanged. Same file as FLUX.1 Schnell declares, so it downloads once.",
                "filename": "ae.safetensors",
                "url": "https://huggingface.co/ffxvs/vae-flux/resolve/main/ae.safetensors",
                "size_bytes": 335304388
              }
            ]
          },
          {
            "id": "black-forest-labs/flux1-schnell",
            "description": "A 12B transformer, roughly three times SDXL's parameters. Handles compound prompts - several subjects, stated positions, a specific style - that smaller models simplify or drop. Four steps.",
            "name": "FLUX.1 Schnell (High Quality)",
            "advanced": ["gpu_id", "allow_fallback", "tile_size", "compression"],
            "default_width": 1024,
            "default_height": 1024,
            "default_steps": 4,
            "default_guidance": 1.0,
            "_defaults_comment": "Schnell is the timestep-distilled FLUX: 1-4 steps, and it ignores CFG entirely (guidance is embedded). Leaving the generic cfg 7.5 in place would be silently meaningless here rather than merely wasteful.",
            "size_gb": 11.41,
            "vram_min_gb": 8,
            "quantizations": ["q4_0"],
            "files": [
              {
                "role": "diffusion",
                "filename": "black-forest-labs_flux1-schnell.gguf",
                "url": "https://huggingface.co/city96/FLUX.1-schnell-gguf/resolve/main/flux1-schnell-Q4_0.gguf",
                "size_bytes": 6770707360
              },
              {
                "role": "clip_l",
                "filename": "clip_l.safetensors",
                "url": "https://huggingface.co/comfyanonymous/flux_text_encoders/resolve/main/clip_l.safetensors",
                "size_bytes": 246144152
              },
              {
                "role": "t5xxl",
                "filename": "t5xxl_fp8_e4m3fn.safetensors",
                "url": "https://huggingface.co/comfyanonymous/flux_text_encoders/resolve/main/t5xxl_fp8_e4m3fn.safetensors",
                "size_bytes": 4893934904
              },
                {
                "role": "vae",
                "filename": "ae.safetensors",
                "_comment": "black-forest-labs/FLUX.1-schnell is a GATED repo (401 GatedRepo without a token). ffxvs/vae-flux is ungated and byte-identical.",
                "url": "https://huggingface.co/ffxvs/vae-flux/resolve/main/ae.safetensors",
                "size_bytes": 335304388
              }
            ]
          },
          {
            "id": "qwen/qwen-image",
            "description": "20B parameters, and the only model here that renders long passages of text legibly - signs, labels, paragraphs, in English or Chinese. Slower and much larger than the rest; worth it when the words in the image matter.",
            "name": "Qwen-Image (Maximum Quality)",
            "advanced": ["gpu_id", "allow_fallback", "tile_size", "compression"],
            "default_width": 1024,
            "default_height": 1024,
            "default_steps": 20,
            "default_guidance": 2.5,
            "_defaults_comment": "cfg 2.5 and 20 steps follow stable-diffusion.cpp's qwen_image.md. Not distilled, so unlike the other three it does use CFG - just at a much lower value than the SD-era 7.5.",
            "_comment": "20B MMDiT conditioned on Qwen2.5-VL 7B. Shares that encoder file byte-for-byte with HunyuanVideo 1.5, so owning either makes the other 4.7 GB smaller. Q4_K_M rather than Q8_0 deliberately: Q8 is 20 GB for the transformer alone, which puts the model out of reach of every machine that could otherwise run it. REGISTERED BUT NEVER RUN HERE.",
            "size_gb": 16.77,
            "vram_min_gb": 8,
            "files": [
              {
                "role": "diffusion",
                "filename": "Qwen_Image-Q4_K_M.gguf",
                "url": "https://huggingface.co/QuantStack/Qwen-Image-GGUF/resolve/main/Qwen_Image-Q4_K_M.gguf",
                "size_bytes": 13065746976
              },
              {
                "role": "llm",
                "_comment": "Same file HunyuanVideo 1.5 declares; the downloader skips it if already verified on disk.",
                "filename": "Qwen2.5-VL-7B-Instruct.Q4_K_M.gguf",
                "url": "https://huggingface.co/mradermacher/Qwen2.5-VL-7B-Instruct-GGUF/resolve/main/Qwen2.5-VL-7B-Instruct.Q4_K_M.gguf",
                "size_bytes": 4683072512
              },
              {
                "role": "vae",
                "_comment": "Qwen-Image has its own VAE; it is not the Flux one.",
                "filename": "Qwen_Image-VAE.safetensors",
                "url": "https://huggingface.co/QuantStack/Qwen-Image-GGUF/resolve/main/VAE/Qwen_Image-VAE.safetensors",
                "size_bytes": 253806246
              }
            ]
          }
        ],
        "image_upscaling": [
          {
            "id": "realesrgan-x4plus",
            "description": "Upscales by 2x or 4x while restoring detail, rather than just interpolating. Tiny, fast, and runs on anything.",
            "name": "Real-ESRGAN x4",
            "advanced": ["tta_mode", "allow_fallback", "compression"],
            "size_gb": 0.01,
            "vram_min_gb": 1,
            "files": [
              {
                "role": "diffusion",
                "filename": "realesrgan-x4plus.gguf",
                "_comment": "The old RealESRGAN_x4plus.gguf path 404s; this is the actual filename published in that repo.",
                "url": "https://huggingface.co/Acly/Real-ESRGAN-GGUF/resolve/main/RealESRGAN-x4plus_anime-6B-F16.gguf",
                "size_bytes": 8950752
              }
            ]
          }
        ],
        "video_generation": [
{
            "id": "wan-ai/wan2.1-t2v-1.3b",
            "description": "The smallest video model that works. Stylised rather than photoreal, but it follows action prompts clearly and is the only one that fits comfortably on a modest machine.",
            "name": "Wan 2.1 T2V 1.3B (Lightest)",
            "advanced": ["gpu_id", "allow_fallback", "tile_size"],
            "frame_alignment": 4,
            "default_fps": 16,
            "default_guidance": 6.0,
            "default_width": 480,
            "default_height": 832,
            "default_negative_prompt": "色调艳丽，过曝，静态，细节模糊不清，字幕，风格，作品，画作，画面，静止，整体发灰，最差质量，低质量，JPEG压缩残留，丑陋的，残缺的，多余的手指，画得不好的手部，画得不好的脸部，畸形的，毁容的，形态畸形的肢体，手指融合，静止不动的画面，杂乱的背景，三条腿，背景人很多，倒着走",
            "_defaults_comment": "From the reference configs in stable-diffusion.cpp docs/wan.md. The generic 7.5 guidance and empty negative prompt we used before are Stable-Diffusion-era values: measured side by side at 320x320/cfg 7.5, Wan 2.2 TI2V 5B produced pure colour noise. Wan was trained with this standard negative prompt and the model cards use it everywhere.",
            "_frame_alignment_comment": "Wan's VAE compresses time 4:1, so stable-diffusion.cpp silently rounds any frame count DOWN to 4n+1 (align_video_frames). The UI snaps to that grid itself so the duration it shows is the duration you get.",
            "_comment": "Smallest architecture stable-diffusion.cpp supports for video. 6.7GB of weights total, which unlike FLUX (15.9GB) leaves headroom on a 16GB machine. Sizes below were read from Content-Length; the downloader verifies them exactly.",
            "size_gb": 6.74,
            "vram_min_gb": 6,
            "files": [
              {
                "role": "diffusion",
                "filename": "wan2.1_t2v_1.3B_fp16.safetensors",
                "url": "https://huggingface.co/Comfy-Org/Wan_2.1_ComfyUI_repackaged/resolve/main/split_files/diffusion_models/wan2.1_t2v_1.3B_fp16.safetensors",
                "size_bytes": 2838303560
              },
              {
                "role": "t5xxl",
                "filename": "umt5-xxl-encoder-Q5_K_M.gguf",
                "_comment": "Q5_K_M over Q8_0 saves 1.8GB of RAM for a barely perceptible quality cost on a machine this tight.",
                "url": "https://huggingface.co/city96/umt5-xxl-encoder-gguf/resolve/main/umt5-xxl-encoder-Q5_K_M.gguf",
                "size_bytes": 4145878880
              },
              {
                "role": "vae",
                "filename": "wan_2.1_vae.safetensors",
                "url": "https://huggingface.co/Comfy-Org/Wan_2.1_ComfyUI_repackaged/resolve/main/split_files/vae/wan_2.1_vae.safetensors",
                "size_bytes": 253815318
              }
            ]
          },
{
            "id": "wan-ai/wan2.2-ti2v-5b",
            "description": "Four times the parameters of the 1.3B and noticeably more photorealistic. Also does image-to-video. The best balance of quality and footprint for most machines.",
            "name": "Wan 2.2 TI2V 5B (Balanced)",
            "advanced": ["gpu_id", "allow_fallback", "tile_size"],
            "frame_alignment": 4,
            "default_fps": 24,
            "default_guidance": 6.0,
            "default_width": 480,
            "default_height": 832,
            "default_negative_prompt": "色调艳丽，过曝，静态，细节模糊不清，字幕，风格，作品，画作，画面，静止，整体发灰，最差质量，低质量，JPEG压缩残留，丑陋的，残缺的，多余的手指，画得不好的手部，画得不好的脸部，畸形的，毁容的，形态畸形的肢体，手指融合，静止不动的画面，杂乱的背景，三条腿，背景人很多，倒着走",
            "_defaults_comment": "From the reference configs in stable-diffusion.cpp docs/wan.md. The generic 7.5 guidance and empty negative prompt we used before are Stable-Diffusion-era values: measured side by side at 320x320/cfg 7.5, Wan 2.2 TI2V 5B produced pure colour noise. Wan was trained with this standard negative prompt and the model cards use it everywhere.",
            "_comment": "Newer architecture, ~4x the parameters of 2.1 1.3B, and TI2V - it does image-to-video as well as text-to-video. Reuses the umt5 encoder the 1.3B model already downloads, so the incremental cost is only the transformer plus the 2.2-specific VAE. NOTE: it needs wan2.2_vae, NOT the 2.1 VAE.",
            "size_gb": 8.72,
            "vram_min_gb": 6,
            "files": [
              {
                "role": "diffusion",
                "filename": "Wan2.2-TI2V-5B-Q5_K_M.gguf",
                "url": "https://huggingface.co/QuantStack/Wan2.2-TI2V-5B-GGUF/resolve/main/Wan2.2-TI2V-5B-Q5_K_M.gguf",
                "size_bytes": 3810603360
              },
              {
                "role": "t5xxl",
                "filename": "umt5-xxl-encoder-Q5_K_M.gguf",
                "_comment": "Shared with wan2.1-t2v-1.3b - already on disk if that model was fetched first.",
                "url": "https://huggingface.co/city96/umt5-xxl-encoder-gguf/resolve/main/umt5-xxl-encoder-Q5_K_M.gguf",
                "size_bytes": 4145878880
              },
              {
                "role": "vae",
                "filename": "wan2.2_vae.safetensors",
                "url": "https://huggingface.co/Comfy-Org/Wan_2.2_ComfyUI_Repackaged/resolve/main/split_files/vae/wan2.2_vae.safetensors",
                "size_bytes": 1409400960
              }
            ]
          },
{
            "id": "wan-ai/wan2.2-t2v-a14b",
            "description": "Two 14B experts, one per denoising phase. The strongest open model for photorealistic people - faces, skin and hair hold up where smaller models break down. Needs a large machine.",
            "name": "Wan 2.2 T2V A14B (Best Photorealism)",
            "advanced": ["gpu_id", "allow_fallback", "tile_size"],
            "frame_alignment": 4,
            "default_fps": 24,
            "default_guidance": 3.5,
            "default_width": 832,
            "default_height": 480,
            "default_steps": 10,
            "default_negative_prompt": "色调艳丽，过曝，静态，细节模糊不清，字幕，风格，作品，画作，画面，静止，整体发灰，最差质量，低质量，JPEG压缩残留，丑陋的，残缺的，多余的手指，画得不好的手部，画得不好的脸部，畸形的，毁容的，形态畸形的肢体，手指融合，静止不动的画面，杂乱的背景，三条腿，背景人很多，倒着走",
            "_comment": "Wan 2.2's mixture-of-experts pair: two 14B transformers, one for the high-noise phase and one for the low. Both must be present - the pipeline passes the second through high_noise_diffusion_model_path. Reviewers rate Wan 2.2 the strongest open model for photorealistic humans (faces, skin, hair). Q4_K_M over Q5 because two experts double every byte. Reuses the umt5 encoder the smaller Wan models already download, so the incremental cost is the two transformers. Guidance 3.5 and 10 steps come from the reference config in stable-diffusion.cpp docs/wan.md, which differ from the single-expert Wan defaults. REGISTERED BUT NEVER RUN HERE.",
            "size_gb": 22.07,
            "vram_min_gb": 16,
            "files": [
              {
                "role": "diffusion",
                "_comment": "Low-noise expert - the one sd.cpp treats as primary.",
                "filename": "Wan2.2-T2V-A14B-LowNoise-Q4_K_M.gguf",
                "url": "https://huggingface.co/QuantStack/Wan2.2-T2V-A14B-GGUF/resolve/main/LowNoise/Wan2.2-T2V-A14B-LowNoise-Q4_K_M.gguf",
                "size_bytes": 9650090496
              },
              {
                "role": "high_noise",
                "filename": "Wan2.2-T2V-A14B-HighNoise-Q4_K_M.gguf",
                "url": "https://huggingface.co/QuantStack/Wan2.2-T2V-A14B-GGUF/resolve/main/HighNoise/Wan2.2-T2V-A14B-HighNoise-Q4_K_M.gguf",
                "size_bytes": 9650090496
              },
              {
                "role": "t5xxl",
                "_comment": "Shared with every other Wan model here.",
                "filename": "umt5-xxl-encoder-Q5_K_M.gguf",
                "url": "https://huggingface.co/city96/umt5-xxl-encoder-gguf/resolve/main/umt5-xxl-encoder-Q5_K_M.gguf",
                "size_bytes": 4145878880
              },
              {
                "role": "vae",
                "_comment": "A14B uses the 2.1 VAE, NOT the 2.2 one that TI2V 5B needs.",
                "filename": "wan_2.1_vae.safetensors",
                "url": "https://huggingface.co/Comfy-Org/Wan_2.1_ComfyUI_repackaged/resolve/main/split_files/vae/wan_2.1_vae.safetensors",
                "size_bytes": 253815318
              }
            ]
          },
{
            "id": "tencent/hunyuanvideo-1.5-t2v",
            "description": "A different architecture entirely, conditioned on a 7B vision-language model. Rated the highest quality ceiling in open source, with the steadiest motion across longer clips.",
            "name": "HunyuanVideo 1.5 (Maximum Quality)",
            "advanced": ["gpu_id", "allow_fallback", "tile_size"],
            "frame_alignment": 4,
            "default_fps": 24,
            "default_guidance": 6.0,
            "default_width": 1280,
            "default_height": 720,
            "default_steps": 30,
            "_negative_prompt_comment": "Deliberately none. Wan's standard negative prompt is a Chinese tag list it was trained against; HunyuanVideo conditions on Qwen2.5-VL, which reads natural language, so that string would be interpreted as content rather than as tags to avoid.",
            "_comment": "A different architecture from Wan, not a bigger Wan: an 8.3B video transformer conditioned on Qwen2.5-VL 7B, with a small ByT5 alongside it for glyph-aware text. Reviewers rate it the highest quality ceiling in open source, particularly for human motion holding together over longer clips. Weights are fp16 because that is what the reference config uses; the fp8_scaled files in the same repo are ComfyUI's scaled variant, and sd.cpp reads plain F8_E4M3 rather than that. REGISTERED BUT NEVER RUN HERE - a 6GB card cannot load it, so nothing about this entry has been confirmed against a real generation.",
            "size_gb": 22.63,
            "vram_min_gb": 16,
            "files": [
              {
                "role": "diffusion",
                "filename": "hunyuanvideo1.5_720p_t2v_fp16.safetensors",
                "url": "https://huggingface.co/Comfy-Org/HunyuanVideo_1.5_repackaged/resolve/main/split_files/diffusion_models/hunyuanvideo1.5_720p_t2v_fp16.safetensors",
                "size_bytes": 16653368128
              },
              {
                "role": "llm",
                "_comment": "Main text conditioning. GGUF rather than the 15GB fp16 safetensors in the same repo.",
                "filename": "Qwen2.5-VL-7B-Instruct.Q4_K_M.gguf",
                "url": "https://huggingface.co/mradermacher/Qwen2.5-VL-7B-Instruct-GGUF/resolve/main/Qwen2.5-VL-7B-Instruct.Q4_K_M.gguf",
                "size_bytes": 4683072512
              },
              {
                "role": "vae",
                "filename": "hunyuanvideo15_vae_fp16.safetensors",
                "url": "https://huggingface.co/Comfy-Org/HunyuanVideo_1.5_repackaged/resolve/main/split_files/vae/hunyuanvideo15_vae_fp16.safetensors",
                "size_bytes": 2521292758
              },
              {
                "role": "t5xxl",
                "_comment": "Glyph-aware conditioning, not the main encoder - HunyuanVideo uses both.",
                "filename": "byt5_small_glyphxl_fp16.safetensors",
                "url": "https://huggingface.co/Comfy-Org/HunyuanVideo_1.5_repackaged/resolve/main/split_files/text_encoders/byt5_small_glyphxl_fp16.safetensors",
                "size_bytes": 438643184
              }
            ]
          },
{
            "id": "minimaxai/minimax-h3-fl2va",
            "description": "Jointly generates video and stereo audio from one packed diffusion transformer - the only model here with sound. A 33B dense architecture conditioned on a 32B vision-language text encoder, larger than anything else in this list.",
            "name": "MiniMax H3 (Video + Audio)",
            "advanced": ["gpu_id", "allow_fallback", "tile_size"],
            "frame_alignment": 17,
            "frame_alignment_offset": 5,
            "default_fps": 24,
            "default_guidance": 1.0,
            "default_width": 864,
            "default_height": 480,
            "_frame_alignment_comment": "Upstream docs/minimax_h3.md: frame count aligns upward to the 17k+5 grid (minimum 5), not Wan's 4k+1 - this is the only registered model using a non-1 alignment offset.",
            "_fps_comment": "MiniMax-H3 runs at a fixed 24fps; the engine overrides any other requested value, per docs/minimax_h3.md.",
            "_comment": "FL2VA checkpoint (text/first-last-frame-to-video); Ref2VA also exists upstream for reference-conditioned generation but is not registered here. Support landed in stable-diffusion.cpp ea7f0c8 and stabilized through 487de75 - see PATCHES.md for why the vendored pin stops there. Diffusion weights are unsloth's Q4_K GGUF quant of the 'pruned' checkpoint; the text encoder is MiniMax-H3's own truncated/exported Qwen3-VL-32B, not a stock Qwen3-VL checkpoint. REGISTERED BUT NEVER RUN HERE - at ~35GB combined and a 33B dense transformer, this is far beyond a 6GB card even with CPU offload; nothing about this entry has been confirmed against a real generation. Omitting the audio_vae file still produces video, without a decoded audio track.",
            "size_gb": 35.45,
            "vram_min_gb": 24,
            "files": [
              {
                "role": "diffusion",
                "filename": "minimax_h3_fl2va_pruned-Q4_K.gguf",
                "url": "https://huggingface.co/unsloth/MiniMax-H3-GGUF/resolve/main/minimax_h3_fl2va_pruned-Q4_K.gguf",
                "size_bytes": 11420663904
              },
              {
                "role": "llm",
                "_comment": "MiniMax-H3's own Qwen3-VL-32B export (truncated to 50 language layers, vision tower and DeepStack mergers included) - not interchangeable with a stock Qwen3-VL checkpoint.",
                "filename": "qwen3vl_32b_minimax_h3-Q4_K_M.gguf",
                "url": "https://huggingface.co/unsloth/MiniMax-H3-GGUF/resolve/main/qwen3vl_32b_minimax_h3-Q4_K_M.gguf",
                "size_bytes": 18218065024
              },
              {
                "role": "vae",
                "filename": "minimax_h3_video_vae_fp16.safetensors",
                "url": "https://huggingface.co/unsloth/MiniMax-H3-GGUF/resolve/main/vae/minimax_h3_video_vae_fp16.safetensors",
                "size_bytes": 5207808496
              },
              {
                "role": "audio_vae",
                "filename": "minimax_h3_audio_vae_fp32.safetensors",
                "url": "https://huggingface.co/unsloth/MiniMax-H3-GGUF/resolve/main/vae/minimax_h3_audio_vae_fp32.safetensors",
                "size_bytes": 605254808
              }
            ]
          }
        ],
        "video_upscaling": [
          {
            "id": "realesrgan-x4plus",
            "description": "Upscales by 2x or 4x while restoring detail, rather than just interpolating. Tiny, fast, and runs on anything.",
            "name": "Real-ESRGAN x4 (per frame)",
            "advanced": ["allow_fallback", "compression"],
            "_comment": "Same weights as the image_upscaling entry - video upscaling runs it frame by frame - so listing it here costs no extra download.",
            "size_gb": 0.01,
            "vram_min_gb": 1,
            "files": [
              {
                "role": "diffusion",
                "filename": "realesrgan-x4plus.gguf",
                "url": "https://huggingface.co/Acly/Real-ESRGAN-GGUF/resolve/main/RealESRGAN-x4plus_anime-6B-F16.gguf",
                "size_bytes": 8950752
              }
            ]
          }
        ]
      }
    })JSON");
}

// Endpoints to try for a given file, in order. huggingface.co is primary; the
// hf-mirror.com rewrite is the fallback for networks that reset connections to
// the primary host (the old Python backend defaulted to the mirror for exactly
// this reason, and the C++ port must not lose that resilience).
std::vector<std::string> url_candidates(const std::string& url) {
    std::vector<std::string> out{url};
    const std::string primary = "huggingface.co";
    const std::string mirror = "hf-mirror.com";
    auto pos = url.find(primary);
    if (pos != std::string::npos) {
        std::string alt = url;
        alt.replace(pos, primary.size(), mirror);
        out.push_back(alt);
    }
    return out;
}

// Finds a model entry by id across every category. Returns null json if absent.
json find_model_entry(const std::string& model_id) {
    json registry = get_default_registry();
    for (const auto& cat : {"image_generation", "image_upscaling", "video_generation", "video_upscaling"}) {
        for (const auto& m : registry["models"][cat]) {
            if (m.value("id", "") == model_id) return m;
        }
    }
    return json();
}

// Filename of the model's primary .gguf, falling back to the legacy
// "<id with / replaced by _>.gguf" convention for anything not in the registry.
std::string primary_filename_for(const std::string& model_id) {
    json entry = find_model_entry(model_id);
    if (!entry.is_null() && entry.contains("files")) {
        for (const auto& f : entry["files"]) {
            if (f.value("role", "") == "diffusion") return f.value("filename", "");
        }
    }
    std::string safe = model_id;
    std::replace(safe.begin(), safe.end(), '/', '_');
    return safe + ".gguf";
}

// True when every file the model declares is present AND passes verification.
// `problem` describes the first failure, for surfacing in the UI.
bool model_is_complete(const std::string& model_id, const std::string& models_dir, std::string& problem) {
    json entry = find_model_entry(model_id);
    if (entry.is_null() || !entry.contains("files")) {
        std::string path = models_dir + "/" + primary_filename_for(model_id);
        return vison::verify_model_file(path, 0, problem);
    }

    for (const auto& f : entry["files"]) {
        std::string filename = f.value("filename", "");
        if (filename.empty()) continue;
        std::string path = models_dir + "/" + filename;
        uint64_t expected = f.value("size_bytes", (uint64_t)0);
        std::string err;
        if (!vison::verify_model_file(path, expected, err)) {
            problem = filename + ": " + err;
            return false;
        }
    }
    problem.clear();
    return true;
}

// A backend that vanishes mid-request is the worst failure mode we have: the
// UI just sees the connection drop, with nothing in any log saying why. This
// happened for real - a GPU device loss during VAE decode killed the process
// silently while tearing down the dead Vulkan context, so the queue's
// device-loss retry never got to run. Whatever kills us from here on says so.
static void install_crash_handlers() {
    // Unbuffer first, or the diagnostics below never reach the log. When stderr
    // is redirected to a file it is fully buffered, so a crash discards up to
    // 4KB of the most recent output - which is exactly the part that explains
    // the crash. This cost real debugging time: the process was dying with an
    // empty tail and no handler output, and the reason was simply that nobody
    // had flushed.
    setvbuf(stderr, nullptr, _IONBF, 0);
    setvbuf(stdout, nullptr, _IONBF, 0);

    std::set_terminate([] {
        std::cerr << "[FATAL] std::terminate called." << std::endl;
        if (auto exc = std::current_exception()) {
            try {
                std::rethrow_exception(exc);
            } catch (const std::exception& e) {
                std::cerr << "[FATAL] Unhandled exception: " << e.what() << std::endl;
            } catch (...) {
                std::cerr << "[FATAL] Unhandled non-standard exception." << std::endl;
            }
        } else {
            std::cerr << "[FATAL] No active exception - most likely an exception thrown "
                         "from a noexcept function (a destructor freeing GPU resources "
                         "after the device was lost is the usual culprit)." << std::endl;
        }
        std::cerr.flush();
        std::abort();
    });

#ifdef _WIN32
    // Structured exceptions (access violations in the graphics driver after a
    // TDR) do not go through std::terminate at all.
    SetUnhandledExceptionFilter([](EXCEPTION_POINTERS* info) -> LONG {
        std::cerr << "[FATAL] Unhandled SEH exception 0x" << std::hex
                  << (info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0)
                  << std::dec << " - the process is going down." << std::endl;
        std::cerr.flush();
        return EXCEPTION_EXECUTE_HANDLER;
    });
#endif
}

int main(int argc, char** argv) {
    install_crash_handlers();

    httplib::Server svr;
    vison::TaskQueue queue;
    
    // Image generation can take 10+ minutes on CPU.
    // Default httplib timeouts are 5 seconds, which kills the connection mid-generation.
    svr.set_read_timeout(30 * 60);   // 30 minutes
    svr.set_write_timeout(30 * 60);  // 30 minutes
    
    // Ensure outputs directory exists
    std::filesystem::create_directories(data_path("outputs"));

    // Serve static files from the outputs directory
    svr.set_mount_point("/outputs", data_path("outputs").c_str());
    
    // CORS Preflight
    svr.Options(R"(.*)", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "POST, GET, PUT, DELETE, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type");
        res.status = 204;
    });

    // Require the launcher's token on every API call.
    //
    // The Electron app mints a random token per launch and passes it in
    // VISON_API_TOKEN, then sends it as a bearer header. Without it, the API is
    // not reachable from a browser tab, a script, or a second copy of the app -
    // which is what "everything behind sign-in" means in practice, because the
    // renderer only gets to make requests once a session exists.
    //
    // With no token in the environment the server stays open. That is the
    // development mode, and also the honest bypass: someone can run this binary
    // themselves without a token. A process on the user's own machine, serving
    // models from the user's own disk, cannot enforce entitlement - this raises
    // the effort, it does not remove the possibility.
    const std::string api_token = [] {
        const char* env = std::getenv("VISON_API_TOKEN");
        return env ? std::string(env) : std::string();
    }();

    if (!api_token.empty()) {
        std::cout << "API token required for /api/* requests" << std::endl;
        svr.set_pre_routing_handler(
            [&api_token](const httplib::Request& req, httplib::Response& res) {
                // Static outputs and CORS preflight stay open: the renderer
                // loads generated images by URL through the <img> tag, which
                // cannot carry an Authorization header.
                if (req.path.rfind("/api/", 0) != 0 || req.method == "OPTIONS") {
                    return httplib::Server::HandlerResponse::Unhandled;
                }

                const std::string expected = "Bearer " + api_token;
                if (req.get_header_value("Authorization") == expected) {
                    return httplib::Server::HandlerResponse::Unhandled;
                }

                res.status = 401;
                res.set_header("Access-Control-Allow-Origin", "*");
                res.set_content(
                    R"({"status":"error","message":"Unauthorized: sign in through the Vison app."})",
                    "application/json");
                return httplib::Server::HandlerResponse::Handled;
            });
    }

    auto add_cors = [](httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
    };
    
    // Live progress channel. The frontend connects here and receives
    // {"type":"progress","step":N,"total":M} during sampling, plus
    // {"type":"generation_status",...} and {"type":"download_progress",...}.
    svr.WebSocket("/api/ws/progress", [&](const httplib::Request&, httplib::ws::WebSocket& ws) {
        {
            std::lock_guard<std::mutex> lock(g_ws_mutex);
            g_ws_clients.push_back(&ws);
        }
        std::cout << "[WS] client connected (" << g_ws_clients.size() << " total)" << std::endl;

        // Hold the connection open. read() blocks until the peer sends
        // something or disconnects; we do not expect inbound messages, so any
        // read failure means the client is gone.
        std::string msg;
        while (ws.is_open()) {
            // ReadResult is Fail(0) / Text(1) / Binary(2); Fail means the peer
            // is gone, which is the only way this loop is expected to end.
            if (ws.read(msg) == httplib::ws::Fail) break;
        }

        {
            std::lock_guard<std::mutex> lock(g_ws_mutex);
            g_ws_clients.erase(std::remove(g_ws_clients.begin(), g_ws_clients.end(), &ws),
                               g_ws_clients.end());
        }
        std::cout << "[WS] client disconnected" << std::endl;
    });

    svr.Get("/api/system", [&](const httplib::Request&, httplib::Response& res) {
        auto device = vison::detect_device();
        auto devices = vison::list_devices();

        // The device list is what makes the gpu_id setting usable: the UI can
        // offer the actual cards by name instead of asking someone to guess an
        // index. "index" here is exactly what gpu_id expects.
        json gpus = json::array();
        for (const auto& d : devices) {
            gpus.push_back({
                {"index", d.index},
                {"name", d.name},
                {"backend", d.backend},
                {"integrated", d.integrated},
                {"vram_bytes", d.vram_bytes},
                {"free_vram_bytes", d.free_vram_bytes},
                {"vram_gb", (double)d.vram_bytes / (1024.0*1024.0*1024.0)}
            });
        }

        json j = {
            {"gpu", device.name},
            {"backend", device.backend},
            {"vram_gb", (double)device.vram_bytes / (1024.0*1024.0*1024.0)},
            {"cuda", device.cuda_available},
            {"vulkan", device.vulkan_available},
            {"devices", gpus},
            // Video needs an external muxer. Without one the pipeline still
            // runs and keeps the PNG frames, but the user gets a folder of
            // stills instead of the clip they asked for - and finding that out
            // after a quarter-hour of generation is not acceptable. Reported
            // here so the UI can say so before anything starts.
            {"ffmpeg", vison::pipelines::ffmpeg_available()},
            {"ffmpeg_path", vison::pipelines::ffmpeg_path()}
        };
        add_cors(res);
        res.set_content(j.dump(), "application/json");
    });
    
    svr.Get("/api/version", [&](const httplib::Request&, httplib::Response& res) {
        json j = {
            {"build_id", std::string(__DATE__) + " " + std::string(__TIME__)}
        };
        add_cors(res);
        res.set_content(j.dump(), "application/json");
    });
    
    svr.Get("/api/models", [&](const httplib::Request&, httplib::Response& res) {
        json response = {
            {"image", json::array()},
            {"video", json::array()},
            {"image_upscale", json::array()},
            {"video_upscale", json::array()}
        };
        
        // Attach a per-model verdict for THIS machine. Working it out from the
        // declared file sizes means the answer is available before anything is
        // downloaded - finding out a 7GB model does not fit by watching it fail
        // is the worst possible way to learn it.
        auto with_compatibility = [](json m) {
            size_t total = 0, encoders = 0;
            if (m.contains("files")) {
                for (const auto& f : m["files"]) {
                    const uint64_t sz = f.value("size_bytes", (uint64_t)0);
                    total += (size_t)sz;
                    const std::string role = f.value("role", "");
                    if (role == "t5xxl" || role == "clip_l" || role == "clip_g" || role == "llm") {
                        encoders += (size_t)sz;
                    }
                }
            }
            auto c = vison::check_compatibility(total, encoders);
            const char* fit = c.fit == vison::Fit::Good          ? "good"
                              : c.fit == vison::Fit::Tight       ? "tight"
                                                                 : "unsupported";
            m["compatibility"] = {
                {"fit", fit},
                {"tier", vison::to_string(c.tier)},
                {"needs_ram_gb", c.needs_ram_gb},
                {"system_ram_gb", c.system_ram_gb},
                {"vram_gb", c.vram_gb},
                {"summary", c.summary}
            };
            return m;
        };

        try {
            json registry = get_default_registry();
            for (const auto& m : registry["models"]["image_generation"]) response["image"].push_back(with_compatibility(m));
            for (const auto& m : registry["models"]["image_upscaling"]) response["image_upscale"].push_back(with_compatibility(m));
            for (const auto& m : registry["models"]["video_generation"]) response["video"].push_back(with_compatibility(m));
            for (const auto& m : registry["models"]["video_upscaling"]) response["video_upscale"].push_back(with_compatibility(m));
        } catch (const std::exception& e) {
            std::cerr << "Error parsing registry: " << e.what() << std::endl;
        }
        
        add_cors(res);
        res.set_content(response.dump(), "application/json");
    });
    
    svr.Get("/api/models/local", [&](const httplib::Request&, httplib::Response& res) {
        // A model counts as "downloaded" only when EVERY file it declares is
        // present and verifies. The old implementation listed any *.gguf and
        // rebuilt the id by replacing every '_' with '/', which both mangled
        // ids containing underscores and reported FLUX as ready when only the
        // transformer had been fetched.
        // present_bytes: how much of each model is ALREADY on disk. Models
        // share files - Qwen-Image and HunyuanVideo declare the same 4.7 GB
        // Qwen2.5-VL encoder, Z-Image and FLUX the same VAE - and the
        // downloader skips anything already verified. Without this the UI can
        // only offer "download 16.8 GB" when the real cost to the user is
        // 12.1 GB, which is the difference between a model looking reachable
        // and looking out of the question.
        json j = { {"downloaded", json::array()},
                   {"incomplete", json::object()},
                   {"present_bytes", json::object()} };
        try {
            std::string models_dir;
            {
                std::lock_guard<std::mutex> lock(g_config_mutex);
                models_dir = g_models_dir;
            }

            json registry = get_default_registry();
            for (const auto& cat : {"image_generation", "image_upscaling", "video_generation", "video_upscaling"}) {
                for (const auto& m : registry["models"][cat]) {
                    std::string id = m.value("id", "");
                    if (id.empty()) continue;
                    std::string problem;
                    // Real-ESRGAN is registered under both image_upscaling and
                    // video_upscaling (same weights, two tasks), so guard against
                    // listing an id twice.
                    const bool already =
                        std::find(j["downloaded"].begin(), j["downloaded"].end(), id) !=
                        j["downloaded"].end();
                    if (already) continue;

                    uint64_t present = 0;
                    if (m.contains("files")) {
                        for (const auto& f : m["files"]) {
                            const std::string name = f.value("filename", "");
                            const uint64_t sz = f.value("size_bytes", (uint64_t)0);
                            if (name.empty()) continue;
                            std::string ignored;
                            if (vison::verify_model_file(models_dir + "/" + name, sz, ignored)) {
                                present += sz;
                            }
                        }
                    }
                    if (present > 0) j["present_bytes"][id] = present;

                    if (model_is_complete(id, models_dir, problem)) {
                        j["downloaded"].push_back(id);
                    } else if (problem.find("missing") == std::string::npos) {
                        // Present but broken (truncated / wrong size) — worth surfacing
                        // so the UI can offer a re-download instead of showing nothing.
                        j["incomplete"][id] = problem;
                    }
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "[models/local] " << e.what() << std::endl;
        }
        add_cors(res);
        res.set_content(j.dump(), "application/json");
    });
    
    auto url_decode = [](const std::string& src) {
        std::string ret; char ch; int i, ii;
        for (i=0; i<src.length(); i++) {
            if (src[i]=='%') {
                if (i + 2 < src.length()) {
                    sscanf(src.substr(i+1,2).c_str(), "%x", &ii);
                    ch=static_cast<char>(ii);
                    ret+=ch; i=i+2;
                }
            } else { ret+=src[i]; }
        }
        return ret;
    };

    // Removes every file a model declares, plus any leftover .part, but skips
    // files that another registry entry also depends on so deleting one model
    // cannot silently break another.
    auto remove_model_files = [](const std::string& model_id) {
        std::string models_dir;
        {
            std::lock_guard<std::mutex> lock(g_config_mutex);
            models_dir = g_models_dir;
        }

        std::set<std::string> shared;
        json registry = get_default_registry();
        for (const auto& cat : {"image_generation", "image_upscaling", "video_generation", "video_upscaling"}) {
            for (const auto& m : registry["models"][cat]) {
                if (m.value("id", "") == model_id) continue;
                if (!m.contains("files")) continue;
                for (const auto& f : m["files"]) shared.insert(f.value("filename", ""));
            }
        }

        json entry = find_model_entry(model_id);
        std::vector<std::string> filenames;
        if (!entry.is_null() && entry.contains("files")) {
            for (const auto& f : entry["files"]) filenames.push_back(f.value("filename", ""));
        } else {
            filenames.push_back(primary_filename_for(model_id));
        }

        std::error_code ec;
        int removed = 0;
        for (const auto& name : filenames) {
            if (name.empty() || shared.count(name)) continue;
            std::string path = models_dir + "/" + name;
            if (std::filesystem::remove(path, ec)) removed++;
            std::filesystem::remove(path + ".part", ec);
        }
        return removed;
    };

    svr.Delete(R"(/api/models/(.*))", [&](const httplib::Request& req, httplib::Response& res) {
        add_cors(res);
        std::string model_id = url_decode(req.matches[1]);
        int removed = remove_model_files(model_id);
        res.set_content(json{{"status", "success"}, {"removed", removed}}.dump(), "application/json");
    });

    svr.Post(R"(/api/models/(.*)/reset-cache)", [&](const httplib::Request& req, httplib::Response& res) {
        add_cors(res);
        std::string model_id = url_decode(req.matches[1]);
        int removed = remove_model_files(model_id);
        res.set_content(json{{"status", "success"}, {"removed", removed}}.dump(), "application/json");
    });

    
    svr.Post("/api/download", [&](const httplib::Request& req, httplib::Response& res) {
        add_cors(res);
        try {
            auto body = json::parse(req.body);
            std::string model_id = body.value("model", "");

            {
                std::lock_guard<std::mutex> lock(g_download.mtx);
                if (g_download.status == "downloading") {
                    json resp = { {"status", "error"}, {"message", "Already downloading"} };
                    res.set_content(resp.dump(), "application/json");
                    return;
                }
            }

            json entry = find_model_entry(model_id);
            if (entry.is_null() || !entry.contains("files") || entry["files"].empty()) {
                json resp = { {"status", "error"}, {"message", "Model not found in registry: " + model_id} };
                res.set_content(resp.dump(), "application/json");
                return;
            }

            std::string models_dir;
            {
                std::lock_guard<std::mutex> cfg_lock(g_config_mutex);
                models_dir = g_models_dir;
                std::filesystem::create_directories(g_models_dir);
            }

            // Snapshot the file list so the worker thread never touches the registry.
            struct FileSpec {
                std::string url;
                std::string dest;
                std::string filename;
                uint64_t size_bytes;
            };
            std::vector<FileSpec> files;
            for (const auto& f : entry["files"]) {
                FileSpec spec;
                spec.filename = f.value("filename", "");
                spec.url = f.value("url", "");
                spec.size_bytes = f.value("size_bytes", (uint64_t)0);
                spec.dest = models_dir + "/" + spec.filename;
                if (!spec.filename.empty() && !spec.url.empty()) files.push_back(spec);
            }

            {
                std::lock_guard<std::mutex> lock(g_download.mtx);
                g_download.status = "downloading";
                g_download.progress = 0.0f;
                g_download.model_id = model_id;
                g_download.error_message.clear();
                g_download.file_count = (int)files.size();
                g_download.file_index = 0;
                g_download.current_file.clear();
                g_download.cancel = false;
            }

            std::thread([files, model_id]() {
                std::cout << "[Download] " << model_id << ": " << files.size() << " file(s)" << std::endl;

                // Weight progress by byte count so the 4.9 GB T5 encoder does not
                // advance the bar at the same rate as the 246 MB CLIP encoder.
                // Files with an unknown size fall back to equal weighting.
                uint64_t known_total = 0;
                int unknown_count = 0;
                for (const auto& f : files) {
                    if (f.size_bytes > 0) known_total += f.size_bytes;
                    else unknown_count++;
                }
                const uint64_t assumed_per_unknown =
                    (known_total && files.size() > (size_t)unknown_count)
                        ? known_total / (files.size() - unknown_count)
                        : 500ull * 1024 * 1024;
                const uint64_t grand_total = known_total + assumed_per_unknown * unknown_count;

                uint64_t completed_bytes = 0;
                bool all_ok = true;
                std::string failure;
                bool was_cancelled = false;

                for (size_t i = 0; i < files.size(); ++i) {
                    const auto& f = files[i];

                    if (g_download.cancel.load()) { was_cancelled = true; break; }

                    // Skip a file that is already present and verifies clean.
                    std::string verify_err;
                    if (vison::verify_model_file(f.dest, f.size_bytes, verify_err)) {
                        std::cout << "[Download] " << f.filename
                                  << " already present and verified, skipping" << std::endl;
                        completed_bytes += (f.size_bytes > 0 ? f.size_bytes : assumed_per_unknown);
                        std::lock_guard<std::mutex> lock(g_download.mtx);
                        g_download.progress = grand_total
                            ? (float)(100.0 * (double)completed_bytes / (double)grand_total)
                            : 0.0f;
                        continue;
                    }

                    {
                        std::lock_guard<std::mutex> lock(g_download.mtx);
                        g_download.current_file = f.filename;
                        g_download.file_index = (int)i + 1;
                    }
                    broadcast_ws(json{
                        {"type", "download_status"},
                        {"model", model_id},
                        {"file", f.filename},
                        {"file_index", (int)i + 1},
                        {"file_count", (int)files.size()}
                    });
                    {
                    }
                    std::cout << "[Download] (" << (i + 1) << "/" << files.size() << ") "
                              << f.filename << " <- " << f.url << std::endl;

                    const uint64_t base = completed_bytes;
                    const uint64_t weight = (f.size_bytes > 0 ? f.size_bytes : assumed_per_unknown);

                    auto on_progress = [base, weight, grand_total](uint64_t got, uint64_t total) {
                        uint64_t scaled = total > 0
                            ? (uint64_t)((double)got / (double)total * (double)weight)
                            : 0;
                        std::lock_guard<std::mutex> lock(g_download.mtx);
                        g_download.progress = grand_total
                            ? (float)(100.0 * (double)(base + scaled) / (double)grand_total)
                            : 0.0f;
                    };

                    // Try the primary URL then the mirror, retrying each with
                    // backoff. Connection resets mid-transfer are common on some
                    // networks; because a failed attempt leaves its .part file in
                    // place, each retry RESUMES rather than restarting, so a
                    // multi-GB file still makes forward progress across resets.
                    constexpr int kAttemptsPerEndpoint = 4;
                    vison::DownloadResult result;
                    bool done = false;
                    for (const auto& candidate : url_candidates(f.url)) {
                        for (int attempt = 1; attempt <= kAttemptsPerEndpoint && !done; ++attempt) {
                            if (g_download.cancel.load()) break;

                            result = vison::download_to_file(candidate, f.dest, on_progress, &g_download.cancel);
                            if (result.success || result.cancelled) { done = true; break; }

                            std::cerr << "[Download] " << f.filename << " attempt " << attempt << "/"
                                      << kAttemptsPerEndpoint << " via " << candidate
                                      << " failed: " << result.error << std::endl;

                            if (attempt < kAttemptsPerEndpoint) {
                                int delay_ms = 1000 * (1 << (attempt - 1));  // 1s, 2s, 4s
                                std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
                            }
                        }
                        if (done) break;
                    }

                    if (result.cancelled) { was_cancelled = true; break; }

                    if (!result.success) {
                        all_ok = false;
                        failure = f.filename + ": " + result.error;
                        break;
                    }

                    // Verify what we just wrote before counting it as done.
                    std::string post_err;
                    if (!vison::verify_model_file(f.dest, f.size_bytes, post_err)) {
                        all_ok = false;
                        failure = f.filename + " failed verification: " + post_err;
                        std::error_code ec;
                        std::filesystem::remove(f.dest, ec);
                        break;
                    }

                    completed_bytes += weight;
                    std::cout << "[Download] " << f.filename << " OK (" << result.bytes
                              << " bytes)" << std::endl;
                }

                std::lock_guard<std::mutex> lock(g_download.mtx);
                g_download.current_file.clear();
                if (was_cancelled) {
                    std::cout << "[Download] Cancelled by user." << std::endl;
                    g_download.status = "idle";
                    g_download.progress = 0.0f;
                } else if (all_ok) {
                    std::cout << "[Download] " << model_id << " complete." << std::endl;
                    g_download.status = "idle";
                    g_download.progress = 100.0f;
                } else {
                    std::cerr << "[Download] FAILED: " << failure << std::endl;
                    g_download.status = "error";
                    g_download.error_message = failure;
                }
            }).detach();

            json resp = { {"status", "started"}, {"files", files.size()} };
            res.set_content(resp.dump(), "application/json");
        } catch (const std::exception& e) {
            std::cerr << "[Download/API] Exception: " << e.what() << std::endl;
            std::lock_guard<std::mutex> lock(g_download.mtx);
            g_download.status = "error";
            g_download.error_message = e.what();
            json resp = { {"status", "error"}, {"message", e.what()} };
            res.set_content(resp.dump(), "application/json");
        }
    });

    svr.Get("/api/download/status", [&](const httplib::Request&, httplib::Response& res) {
        add_cors(res);
        std::lock_guard<std::mutex> lock(g_download.mtx);
        json resp = {
            {"status", g_download.status},
            {"model_id", g_download.model_id},
            {"progress", g_download.progress},
            {"error_message", g_download.error_message},
            {"current_file", g_download.current_file},
            {"file_index", g_download.file_index},
            {"file_count", g_download.file_count}
        };
        // Legacy field: keep the old { model_id: true } for backward compat
        if (g_download.status == "downloading" && !g_download.model_id.empty()) {
            resp[g_download.model_id] = true;
        }
        res.set_content(resp.dump(), "application/json");
    });

    svr.Post("/api/download/cancel", [&](const httplib::Request&, httplib::Response& res) {
        add_cors(res);
        std::lock_guard<std::mutex> lock(g_download.mtx);
        g_download.cancel = true;
        json resp = { {"status", "success"} };
        res.set_content(resp.dump(), "application/json");
    });

    // NOTE: DELETE /api/models/(..) and POST /api/models/(..)/reset-cache
    // are already registered above (with proper URL decoding).
    // Duplicate handlers were removed to avoid shadowing.

    svr.Post("/api/config", [&](const httplib::Request& req, httplib::Response& res) {
        add_cors(res);
        try {
            auto body = json::parse(req.body);
            std::string new_dir = body.value("models_dir", "models");
            {
                std::lock_guard<std::mutex> lock(g_config_mutex);
                g_models_dir = new_dir;
            }
            json resp = { {"status", "success"} };
            res.set_content(resp.dump(), "application/json");
        } catch (...) {
            json resp = { {"status", "error"} };
            res.set_content(resp.dump(), "application/json");
        }
    });

    svr.Post("/api/generate", [&](const httplib::Request& req, httplib::Response& res) {
        add_cors(res);

        std::string temp_init_image;  // cleaned up before returning
        auto cleanup_temp = [&temp_init_image]() {
            if (!temp_init_image.empty()) {
                std::error_code ec;
                std::filesystem::remove(temp_init_image, ec);
            }
        };

        try {
            auto body = json::parse(req.body);

            vison::GenerateParams params;
            params.prompt = body.value("prompt", "");
            params.negative_prompt = body.value("negative_prompt", "");

            std::string task_str = body.value("task", "image");
            if (task_str == "image") params.task = vison::TaskType::IMAGE_GENERATION;
            else if (task_str == "image_upscale") params.task = vison::TaskType::IMAGE_UPSCALING;
            else if (task_str == "video") params.task = vison::TaskType::VIDEO_GENERATION;
            else if (task_str == "video_upscale") params.task = vison::TaskType::VIDEO_UPSCALING;

            std::string model_id = body.value("model", "unknown");

            std::string models_dir;
            {
                std::lock_guard<std::mutex> cfg_lock(g_config_mutex);
                models_dir = g_models_dir;
            }
            params.model_path = models_dir + "/" + primary_filename_for(model_id);

            // Hand the pipeline every companion file this model declares, so it
            // never has to guess which VAE or encoder beside it belongs to it.
            json entry = find_model_entry(model_id);
            {
                if (!entry.is_null() && entry.contains("files")) {
                    for (const auto& f : entry["files"]) {
                        const std::string role = f.value("role", "");
                        const std::string name = f.value("filename", "");
                        if (role.empty() || name.empty()) continue;
                        params.model_files[role] = models_dir + "/" + name;
                    }
                }
            }

            // Pre-flight: verify EVERY file the model needs, not just the .gguf.
            // For FLUX this catches a missing text encoder or VAE here, with a
            // clear message, instead of as an opaque load failure minutes later.
            {
                std::string problem;
                if (!model_is_complete(model_id, models_dir, problem)) {
                    cleanup_temp();
                    json resp = {
                        {"status", "error"},
                        {"message", "Model '" + model_id + "' is not ready: " + problem +
                                    ". Download (or re-download) the model first."}
                    };
                    res.set_content(resp.dump(), "application/json");
                    return;
                }
            }

            // Per-model defaults beat generic ones. 512x512 and guidance 7.5 are
            // Stable-Diffusion-era numbers; a Wan video model run at those
            // settings produces colour noise, which is exactly what a side-by-side
            // test of Wan 2.2 TI2V 5B produced before this existed. A model that
            // declares nothing keeps the old behaviour.
            auto reg_int = [&entry](const char* key, int fallback) {
                return (!entry.is_null() && entry.contains(key) && entry[key].is_number())
                           ? entry[key].get<int>() : fallback;
            };
            auto reg_float = [&entry](const char* key, float fallback) {
                return (!entry.is_null() && entry.contains(key) && entry[key].is_number())
                           ? entry[key].get<float>() : fallback;
            };

            params.width  = body.value("width",  reg_int("default_width",  512));
            params.height = body.value("height", reg_int("default_height", 512));
            params.steps = body.value("num_inference_steps", reg_int("default_steps", 20));
            params.guidance = body.value("guidance_scale", reg_float("default_guidance", 7.5f));

            // An absent OR empty negative prompt falls back to the model's own.
            // Wan ships a standard negative prompt that its model cards use
            // everywhere and that measurably improves output; treating "" as
            // "the user has no opinion" is what makes it reach them. Typing
            // anything at all still wins.
            if (params.negative_prompt.empty() && !entry.is_null() &&
                entry.contains("default_negative_prompt")) {
                params.negative_prompt = entry["default_negative_prompt"].get<std::string>();
            }
            params.seed = body.value("seed", -1);
            params.output_format = body.value("output_format", "png");
            params.strength = body.value("strength", 0.75f);

            // Upscale specifics
            params.upscale_quality = body.value("upscale_quality", "4x");

            // Advanced settings. Each model declares which of these it actually
            // supports (see "advanced" in the registry) and the UI only shows
            // those, but a direct API caller can send anything - a pipeline
            // that does not support a field just ignores it.
            params.compression    = body.value("compression", 0);
            params.gpu_id         = body.value("gpu_id", std::string());
            params.allow_fallback = body.value("allow_fallback", false);
            params.tile_size      = body.value("tile_size", 0);
            params.tta_mode       = body.value("tta_mode", false);

            // Video generation length/rate.
            params.video_frames = body.value("video_frames", 33);
            params.fps          = body.value("fps", reg_int("default_fps", 16));

            // img2img: the UI sends the source as a data: URL. Decode it to a
            // temp file so the pipeline can hand it to visp::image_load, which
            // already handles every container we support.
            std::string base_image = body.value("base_image", "");
            if (!base_image.empty()) {
                auto comma = base_image.find(',');
                std::string b64 = (base_image.rfind("data:", 0) == 0 && comma != std::string::npos)
                                      ? base_image.substr(comma + 1)
                                      : base_image;

                std::string decoded;
                if (!base64_decode(b64, decoded) || decoded.empty()) {
                    cleanup_temp();
                    json resp = { {"status", "error"}, {"message", "base_image is not valid base64"} };
                    res.set_content(resp.dump(), "application/json");
                    return;
                }

                std::filesystem::create_directories(data_path("outputs"));
                // Name the scratch file for what it actually holds. ffmpeg and
                // stb both sniff content rather than trusting the extension, but
                // a .png full of mp4 is a trap for the next person reading this.
                const bool attachment_is_video =
                    params.task == vison::TaskType::VIDEO_UPSCALING ||
                    params.task == vison::TaskType::VIDEO_GENERATION;
                temp_init_image = data_path("outputs/_init_" + std::to_string(next_output_id())) +
                                  (attachment_is_video ? ".mp4" : ".png");
                std::ofstream f(temp_init_image, std::ios::binary);
                f.write(decoded.data(), (std::streamsize)decoded.size());
                f.close();

                // Generation treats the attachment as an img2img starting
                // point; upscaling treats it as the image to enlarge. Those are
                // two different fields on GenerateParams, so route the decoded
                // file to the one this task actually reads. Upscaling used to
                // get only init_image_path set and therefore always failed with
                // "Input image not found: ".
                if (params.task == vison::TaskType::IMAGE_UPSCALING ||
                    params.task == vison::TaskType::VIDEO_UPSCALING) {
                    params.input_image_path = temp_init_image;
                } else {
                    // Both image and video generation treat it as the frame to
                    // start from - img2img for stills, image-to-video for a
                    // TI2V/I2V model.
                    params.init_image_path = temp_init_image;
                }
            }

            // Upscaling without a source image can never succeed, so say so up
            // front instead of loading the model first and failing afterwards.
            if ((params.task == vison::TaskType::IMAGE_UPSCALING ||
                 params.task == vison::TaskType::VIDEO_UPSCALING) &&
                params.input_image_path.empty()) {
                cleanup_temp();
                json resp = {
                    {"status", "error"},
                    {"message", "Upscaling needs an image. Attach one and try again."}
                };
                res.set_content(resp.dump(), "application/json");
                return;
            }

            // Unique output name so concurrent or rapid requests cannot clobber
            // each other (the old code always wrote "output_gen.png").
            std::filesystem::create_directories(data_path("outputs"));
            params.output_path = data_path("outputs/gen_" + std::to_string(next_output_id()) + "." +
                                           params.output_format);

            // Stream step progress to any connected WebSocket clients.
            params.on_progress = [](int step, int total) {
                broadcast_ws(json{
                    {"type", "progress"},
                    {"step", step},
                    {"total", total}
                });
            };

            broadcast_ws(json{{"type", "generation_status"}, {"status", "started"}, {"model", model_id}});

            // Wait for generation to complete using a promise
            std::promise<vison::GenerateResult> promise;
            auto future = promise.get_future();

            queue.enqueue(params, [&promise](const vison::GenerateResult& r) {
                promise.set_value(r);
            });

            auto result = future.get();
            cleanup_temp();

            json resp;
            if (result.success) {
                std::filesystem::path p(result.output_path);

                // The pipeline writes straight into outputs/ now, but a pipeline
                // that ignored output_path would leave the file elsewhere.
                std::string target_path = data_path("outputs/" + p.filename().string());
                if (p.string() != target_path) {
                    std::error_code ec;
                    std::filesystem::rename(p, target_path, ec);
                }

                resp["status"] = "success";
                resp["image_url"] = "http://127.0.0.1:11439/outputs/" + p.filename().string();
                resp["elapsed_seconds"] = result.elapsed_seconds;

                broadcast_ws(json{
                    {"type", "generation_status"},
                    {"status", "completed"},
                    {"image_url", resp["image_url"]},
                    {"elapsed_seconds", result.elapsed_seconds}
                });
            } else if (result.error_message.find("CancelledByUser") != std::string::npos ||
                       result.error_message.find("Cancelled") != std::string::npos) {
                resp["status"] = "cancelled";
                resp["message"] = "Generation cancelled by user.";
                broadcast_ws(json{{"type", "generation_status"}, {"status", "cancelled"}});
            } else {
                resp["status"] = "error";
                resp["message"] = result.error_message;
                broadcast_ws(json{
                    {"type", "generation_status"},
                    {"status", "error"},
                    {"message", result.error_message}
                });
            }

            res.set_content(resp.dump(), "application/json");

        } catch (const std::exception& e) {
            cleanup_temp();
            json resp = { {"status", "error"}, {"message", e.what()} };
            res.set_content(resp.dump(), "application/json");
            broadcast_ws(json{{"type", "generation_status"}, {"status", "error"}, {"message", std::string(e.what())}});
        }
    });

    svr.Post("/api/generate/cancel", [&](const httplib::Request&, httplib::Response& res) {
        add_cors(res);
        // Real cancellation: this reaches into the running pipeline and calls
        // sd_cancel_generation(), and also drops anything still queued.
        bool was_running = queue.cancel_current();
        json resp = {
            {"status", was_running ? "cancelling" : "idle"},
            {"was_running", was_running}
        };
        res.set_content(resp.dump(), "application/json");
    });

    svr.Post("/api/models/unload", [&](const httplib::Request&, httplib::Response& res) {
        add_cors(res);
        queue.unload_cached_model();
        res.set_content(json{{"status", "success"}}.dump(), "application/json");
    });

    svr.Get("/api/diagnostics/connectivity", [&](const httplib::Request&, httplib::Response& res) {
        add_cors(res);

        const std::vector<std::pair<std::string, std::string>> probes = {
            {"huggingface_api",    "https://huggingface.co/api/models/city96/FLUX.1-schnell-gguf"},
            {"huggingface_file",   "https://huggingface.co/comfyanonymous/flux_text_encoders/resolve/main/clip_l.safetensors"},
            {"mirror_api",         "https://hf-mirror.com/api/models/city96/FLUX.1-schnell-gguf"},
            {"mirror_file",        "https://hf-mirror.com/comfyanonymous/flux_text_encoders/resolve/main/clip_l.safetensors"},
        };

        json results = json::array();
        std::map<std::string, vison::ProbeResult> by_name;
        for (const auto& [name, url] : probes) {
            auto r = vison::probe_endpoint(url);
            by_name[name] = r;
            results.push_back({
                {"name", name},
                {"url", url},
                {"ok", r.ok},
                {"status", r.status},
                {"got_byte", r.got_byte},
                {"seconds", r.seconds},
                {"error", r.error}
            });
        }

        auto transfers = [&](const char* n) { return by_name[n].ok && by_name[n].got_byte; };
        const bool hf_api = by_name["huggingface_api"].ok;
        const bool hf_file = transfers("huggingface_file");
        const bool mirror_api = by_name["mirror_api"].ok;
        const bool mirror_file = transfers("mirror_file");

        std::string summary;
        if (hf_file && mirror_file) {
            summary = "both huggingface.co and the mirror are transferring files normally";
        } else if (!hf_file && mirror_file) {
            summary = "huggingface.co is blocked or resetting on this network; the hf-mirror.com "
                      "fallback is healthy and downloads will use it automatically";
        } else if (hf_file && !mirror_file) {
            summary = "huggingface.co works; the mirror is unreachable (fine, it is only a fallback)";
        } else if (hf_api || mirror_api) {
            summary = "API endpoints respond but file transfer is being blocked or reset — "
                      "downloads will retry and resume, but may be slow or fail";
        } else {
            summary = "no connectivity to huggingface.co or hf-mirror.com";
        }

        json j = { {"results", results}, {"summary", summary} };
        res.set_content(j.dump(), "application/json");
    });

    
    svr.Post("/api/shutdown", [&](const httplib::Request&, httplib::Response& res) {
        add_cors(res);
        json resp = { {"status", "shutting down"} };
        res.set_content(resp.dump(), "application/json");
        std::thread([&svr]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            svr.stop();
        }).detach();
    });
    
    std::cout << "Vison Server starting on http://127.0.0.1:11439" << std::endl;
    svr.listen("127.0.0.1", 11439);
    return 0;
}
