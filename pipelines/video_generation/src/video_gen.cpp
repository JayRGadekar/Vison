#include "vison/pipelines/video_gen.h"
#include "stable-diffusion.h"
#include "visp/image.h"
#include "vison/pipelines/image_output.h"
#include "vison/pipelines/video_io.h"
#include "vison/pipelines/backend_error.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace vison::pipelines {

class VideoGenerationPipeline : public Pipeline {
public:
    VideoGenerationPipeline() {}

    ~VideoGenerationPipeline() override {
        unload_model();
    }

    // Same validation the image pipeline applies: gpu_id is free text from an
    // HTTP request that ends up inside a backend spec string.
    static std::string sanitize_device_index(const std::string& gpu_id) {
        if (gpu_id.empty()) return "0";
        if (gpu_id.size() > 2 || gpu_id.find_first_not_of("0123456789") != std::string::npos) {
            std::cerr << "[Vison] Ignoring unusable gpu_id '" << gpu_id
                      << "'; expected a device index like \"0\". Using device 0." << std::endl;
            return "0";
        }
        return gpu_id;
    }

    // Finds the first file in `dir` whose name contains every fragment given.
    // The video checkpoints are published under several naming schemes
    // (umt5_xxl_fp16.safetensors, umt5-xxl-encoder-Q8_0.gguf, ...), so matching
    // on fragments beats listing every permutation.
    static std::string find_companion(const std::filesystem::path& dir,
                                      std::initializer_list<const char*> fragments) {
        std::error_code ec;
        if (!std::filesystem::exists(dir, ec)) return {};
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file(ec)) continue;
            std::string name = entry.path().filename().string();
            std::string lower;
            for (char c : name) lower.push_back((char)std::tolower((unsigned char)c));

            bool all = true;
            for (const char* f : fragments) {
                if (lower.find(f) == std::string::npos) { all = false; break; }
            }
            if (all) return entry.path().string();
        }
        return {};
    }

// Turns "it failed" into something the user can act on. stable-diffusion.cpp
// only signals failure with a false return; the reason is in the log, so pull
// whatever the backend actually complained about back out of it.
static std::string describe_failure(const char* generic) {
    const std::string detail = BackendErrorLog::instance().take();
    if (detail.empty()) return generic;

    if (is_out_of_memory_error(detail)) {
        return std::string(generic) +
               ": the GPU ran out of memory (" + detail + "). "
               "Reduce the resolution, the number of frames, or the step count - "
               "or set VISON_VRAM_PROFILE=stream to trade speed for a smaller footprint.";
    }
    return std::string(generic) + ": " + detail;
}

    bool load_model(const std::string& gguf_path, const GenerateParams& load_params) override {
        if (!std::filesystem::exists(gguf_path)) {
            std::cerr << "Model file not found: " << gguf_path << std::endl;
            return false;
        }

        sd_ctx_params_t params;
        sd_ctx_params_init(&params);

        const std::filesystem::path model_dir = std::filesystem::path(gguf_path).parent_path();

        // Video checkpoints are split the same way FLUX is: the file we are
        // handed holds only the diffusion transformer, and the text encoder and
        // VAE live beside it. Passing it as model_path would make
        // stable-diffusion.cpp read it as a full checkpoint and fail to bind
        // anything.
        // Prefer what the model actually declared. Falling back to a directory
        // scan keeps a hand-placed model working, but the scan cannot tell
        // wan_2.1_vae.safetensors from wan2.2_vae.safetensors when both are
        // present - it would take whichever came first and quietly pair the
        // wrong VAE with the model.
        auto declared = [&](const char* role) -> std::string {
            auto it = load_params.model_files.find(role);
            if (it == load_params.model_files.end()) return {};
            std::error_code ec;
            return std::filesystem::exists(it->second, ec) ? it->second : std::string{};
        };

        t5xxl_path_ = declared("t5xxl");
        if (t5xxl_path_.empty()) t5xxl_path_ = find_companion(model_dir, {"umt5"});

        vae_path_ = declared("vae");
        if (vae_path_.empty()) vae_path_ = find_companion(model_dir, {"vae", ".safetensors"});

        // Newer architectures condition on a full language model rather than a
        // T5. HunyuanVideo 1.5 uses Qwen2.5-VL 7B here (and keeps a small ByT5
        // in the t5xxl slot for glyph-aware text); LTX-2 uses Gemma-3. Purely
        // declarative - a model that does not list an "llm" file leaves the
        // slot null and loads exactly as before.
        llm_path_ = declared("llm");

        // The T5 slot is required for Wan, but a model conditioning on an LLM
        // may legitimately have no T5 at all, so the requirement is "some text
        // encoder", not "a T5".
        if ((t5xxl_path_.empty() && llm_path_.empty()) || vae_path_.empty()) {
            std::cerr << "[Vison] ERROR: video model needs a text encoder (umt5 or llm) and a VAE "
                      << "beside " << gguf_path << ". Found t5xxl='" << t5xxl_path_
                      << "' llm='" << llm_path_ << "' vae='" << vae_path_ << "'." << std::endl;
            return false;
        }

        params.diffusion_model_path = gguf_path.c_str();
        params.model_path           = "";
        params.t5xxl_path           = t5xxl_path_.c_str();
        params.vae_path             = vae_path_.c_str();
        if (!llm_path_.empty()) params.llm_path = llm_path_.c_str();

        // Wan 2.2 ships two experts; the second is optional and only used when
        // it is actually present next to the first.
        high_noise_path_ = declared("high_noise");
        if (high_noise_path_.empty()) high_noise_path_ = find_companion(model_dir, {"high_noise"});
        if (!high_noise_path_.empty() && high_noise_path_ != gguf_path) {
            params.high_noise_diffusion_model_path = high_noise_path_.c_str();
        }

        std::cerr << "[Vison] Video split load: diffusion=" << gguf_path
                  << " t5xxl=" << t5xxl_path_ << " vae=" << vae_path_
                  << (llm_path_.empty() ? "" : (" llm=" + llm_path_))
                  << (high_noise_path_.empty() ? "" : (" high_noise=" + high_noise_path_))
                  << std::endl;

#if defined(VISON_CUDA)
        params.flash_attn = true;
        params.diffusion_flash_attn = true;
        static const char* cuda_backend = "cuda";
        params.backend = cuda_backend;
        std::cerr << "[Vison] CUDA backend requested" << std::endl;
#elif defined(VISON_VULKAN)
        params.flash_attn = true;
        params.diffusion_flash_attn = true;

        // Placement is planned from the real device, exactly as in image
        // generation - see the note there. Video is the harder case: Wan's VAE
        // alone asks for a ~3.1GB compute buffer, so a small card needs every
        // bit of the streaming strategy while a large one should skip it.
        const ModelFootprint footprint =
            measure_footprint({gguf_path, t5xxl_path_, vae_path_, high_noise_path_, llm_path_},
                              {t5xxl_path_, llm_path_});

        // Conservative ceiling for video: its graph segments carry a time
        // dimension, so the same budget buys a much longer submit than a still
        // image's, and a submit past ~2s trips the Windows GPU watchdog.
        //
        // Honest about the evidence: this is a defensible default, NOT a proven
        // fix. On a 6GB card sharing the GPU with a desktop, 9-frame clips fail
        // intermittently at 4 GiB *and* at 2 GiB, and capping
        // GGML_VK_MAX_NODES_PER_SUBMIT to 16 did not help either - 2 successes
        // in 7 attempts overall. Both successes were the first run after a
        // restart, when free VRAM is highest, which points at contention with
        // whatever else is holding VRAM rather than at any single knob here.
        constexpr size_t kVideoBudgetCeilingGiB = 2;
        plan_ = plan_backend(footprint, sanitize_device_index(load_params.gpu_id),
                             load_params.vram_backoff, kVideoBudgetCeilingGiB);

        if (const char* env = std::getenv("VISON_BACKEND_SPEC")) plan_.backend_spec = env;
        if (const char* env = std::getenv("VISON_MAX_VRAM")) plan_.max_vram = env;

        backend_spec_   = plan_.backend_spec;
        params_backend_ = plan_.params_backend;
        vram_budget_    = plan_.max_vram;

        params.backend        = backend_spec_.c_str();
        params.params_backend = params_backend_.empty() ? nullptr : params_backend_.c_str();
        params.max_vram       = vram_budget_.empty() ? nullptr : vram_budget_.c_str();
        params.stream_layers  = plan_.stream_layers;

        // Report what is actually in effect, not what was planned: the env
        // overrides above are applied after plan_backend() built its summary,
        // so printing that string alone claims a budget we may not be using.
        std::cerr << "[Vison] Backend plan: " << plan_.summary << std::endl;
        std::cerr << "[Vison] Effective: backend='" << backend_spec_
                  << "' params_backend='" << (params_backend_.empty() ? "<device>" : params_backend_)
                  << "' max_vram=" << (vram_budget_.empty() ? "<uncapped>" : vram_budget_ + " GiB")
                  << " stream_layers=" << (plan_.stream_layers ? "on" : "off")
                  << " vae_tiling=" << (plan_.vae_tiling ? "on" : "off") << std::endl;
#else
        std::cerr << "[Vison] Using default (CPU) backend" << std::endl;
#endif

        params.enable_mmap = std::getenv("VISON_NO_MMAP") == nullptr;

        sd_set_log_callback([](enum sd_log_level_t, const char* text, void*) {
            std::cerr << text;
            if (text) BackendErrorLog::instance().observe(text);
        }, nullptr);

        sd_ctx_ = new_sd_ctx(&params);

        if (sd_ctx_ == nullptr && load_params.allow_fallback) {
            std::cerr << "[Vison] GPU context could not be created and allow_fallback is on; "
                         "retrying on the CPU. Video on a CPU is extremely slow."
                      << std::endl;
            backend_spec_ = "all=cpu";
            params.backend        = backend_spec_.c_str();
            params.params_backend = nullptr;
            params.max_vram       = nullptr;
            params.stream_layers  = false;
            sd_ctx_ = new_sd_ctx(&params);
        }

        if (sd_ctx_ == nullptr) {
            std::cerr << "Failed to create sd_ctx for video model: " << gguf_path << std::endl;
            return false;
        }

        is_loaded_ = true;
        return true;
    }

    void unload_model() override {
        if (!is_loaded_ || !sd_ctx_) return;

        sd_ctx_t* ctx = sd_ctx_;
        sd_ctx_ = nullptr;
        is_loaded_ = false;

        // Freeing a context waits on the GPU one last time, which throws if the
        // device was reset. This runs from a destructor, so an escaping
        // exception would call std::terminate and take the server down.
        try {
            free_sd_ctx(ctx);
        } catch (const std::exception& e) {
            std::cerr << "[Vison] Ignoring error while releasing the GPU context: "
                      << e.what() << std::endl;
        } catch (...) {
            std::cerr << "[Vison] Ignoring unknown error while releasing the GPU context"
                      << std::endl;
        }
    }

    bool is_loaded() const override {
        return is_loaded_;
    }

    void request_cancel() override {
        if (sd_ctx_) {
            cancelled_ = true;
            sd_cancel_generation(sd_ctx_, SD_CANCEL_ALL);
            std::cerr << "[Vison] Cancellation requested" << std::endl;
        }
    }

    GenerateResult run(const GenerateParams& params) override {
        GenerateResult result;
        result.success = false;

        if (!is_loaded_ || !sd_ctx_) {
            result.error_message = "Model not loaded";
            return result;
        }

        sd_cancel_generation(sd_ctx_, SD_CANCEL_RESET);
        cancelled_ = false;
        BackendErrorLog::instance().clear();

        sd_vid_gen_params_t gen_params;
        sd_vid_gen_params_init(&gen_params);
        gen_params.prompt = params.prompt.c_str();
        gen_params.negative_prompt = params.negative_prompt.c_str();
        gen_params.width = params.width;
        gen_params.height = params.height;
        gen_params.sample_params.sample_steps = params.steps;
        gen_params.sample_params.guidance.txt_cfg = params.guidance;
        gen_params.seed = params.seed;
        // Previously left at whatever the defaults were, so the frame count and
        // playback rate the caller asked for were silently ignored.
        gen_params.video_frames = params.video_frames > 0 ? params.video_frames : 33;
        gen_params.fps          = params.fps > 0 ? params.fps : 16;

#if defined(VISON_VULKAN) || defined(VISON_CUDA)
        // The Wan VAE is famously VRAM-hungry, and it decodes every frame -
        // tiling it is what keeps a single submit inside the GPU watchdog.
        gen_params.vae_tiling_params.enabled = plan_.vae_tiling;
        gen_params.vae_tiling_params.target_overlap = 0.25f;

        // Choose how many tiles to cut the frame into, rather than how big each
        // tile is.
        //
        // sd.cpp's tile size is expressed in LATENT units and then clamped to
        // the latent dimension (vae.hpp get_tile_sizes). That clamp is a trap
        // for video: Wan 2.2's VAE compresses space 16x, so a 320x320 request
        // is a 20x20 latent, smaller than the default 32x32 tile - so it
        // decided one tile covered everything and tried to decode all 33 frames
        // in a single graph. Measured: a 7.8 GiB allocation on a 5.8 GiB card.
        // Wan 2.1 compresses 8x, so the same request tiled 2x2 and survived.
        // Anything keyed to the latent size inherits that per-VAE behaviour.
        //
        // rel_size_* > 1 means "this many tiles across", which sd.cpp converts
        // to a fraction itself. That is compression-ratio independent, so it
        // behaves the same on both VAEs and on whatever ships next.
        //
        // The count comes from the decoded PIXEL area of ONE TILE. Frame count
        // deliberately does not appear: measured, the buffer barely moves with
        // it, because the latent temporal dimension is small (Wan compresses
        // time 4:1, so 33 frames is 9 latent steps) and the graph reuses
        // buffers across it. An earlier version of this scaled by frames and
        // got it badly wrong - it sized a 5-frame clip at 1298 MB when the
        // allocator went on to ask for 6752 MB.
        //
        // Measured MB per output pixel of one tile:
        //   Wan 2.2  320x320 33f 2x2 ->  1636 MB over 25,600 px = 0.064
        //   Wan 2.2  480x832 33f 4x4 ->  2805 MB over 24,960 px = 0.112
        //   Wan 2.2  480x832  5f 2x2 ->  6752 MB over 99,840 px = 0.068  (OOM)
        //   Wan 2.1  320x320 33f 2x2 ->  4424 MB over 25,600 px = 0.173  (device lost)
        // 0.15 sits just under the worst of those. Erring high costs some
        // speed; erring low costs a failed decode after minutes of diffusion,
        // and that asymmetry is what the margin buys.
        {
            constexpr double kMbPerTilePixel = 0.15;   // MB per output px of a tile

            // Budget: what the card can actually spare right now, not its
            // nameplate size. The desktop compositor and the browser are
            // already holding VRAM, and that is exactly the contention that
            // turns a marginal allocation into a device loss.
            double budget_mb = 0;
            for (const auto& dev : list_devices()) {
                if (dev.backend != "vulkan" && dev.backend != "cuda") continue;
                const size_t avail = dev.free_vram_bytes > 0 ? dev.free_vram_bytes : dev.vram_bytes;
                budget_mb = (double)avail / (1024.0 * 1024.0);
                break;
            }
            if (budget_mb <= 0) budget_mb = 2048;    // no device info: assume a small card
            budget_mb *= 0.60;                        // leave room for weights and the driver

            // Each extra backoff level halves the budget, so a device loss
            // retries with genuinely shorter submits instead of the same plan.
            for (int i = 0; i < params.vram_backoff; ++i) budget_mb *= 0.5;

            const double needed_mb =
                kMbPerTilePixel * (double)gen_params.width * (double)gen_params.height;

            // n tiles per axis -> each tile is 1/n of the width and 1/n of the
            // height, so the per-tile cost falls as n^2.
            int tiles = 1;
            while (tiles < 8 && needed_mb / (double)(tiles * tiles) > budget_mb) ++tiles;

            if (tiles > 1) {
                gen_params.vae_tiling_params.rel_size_x = (float)tiles;
                gen_params.vae_tiling_params.rel_size_y = (float)tiles;
            }
            std::cerr << "[Vison] VAE decode: " << gen_params.width << "x" << gen_params.height
                      << "x" << gen_params.video_frames << " needs ~" << (int)needed_mb
                      << " MB untiled, budget ~" << (int)budget_mb << " MB -> "
                      << tiles << "x" << tiles << " tiles (~"
                      << (int)(needed_mb / (double)(tiles * tiles)) << " MB each)" << std::endl;

            // An explicit tile size from the caller still wins - it is an
            // escape hatch for when this estimate is wrong.
            if (params.tile_size > 0) {
                const int latent_tile = params.tile_size / 8 < 4 ? 4 : params.tile_size / 8;
                gen_params.vae_tiling_params.tile_size_x = latent_tile;
                gen_params.vae_tiling_params.tile_size_y = latent_tile;
                gen_params.vae_tiling_params.rel_size_x = 0.f;
                gen_params.vae_tiling_params.rel_size_y = 0.f;
            }
        }
#endif

        // Image-to-video. sd_vid_gen_params_t has always had init_image, but
        // nothing filled it, so the "I" half of a TI2V model was unreachable -
        // attaching a picture to a video request silently generated from noise
        // instead of animating it. visp owns the pixels, so it must outlive
        // generate_video().
        visp::image_data init_img;
        if (!params.init_image_path.empty() && std::filesystem::exists(params.init_image_path)) {
            try {
                init_img = visp::image_load(params.init_image_path.c_str());
                visp::image_view iv(init_img);
                const int channels = visp::n_channels(iv);
                if (channels == 3 || channels == 4) {
                    gen_params.init_image.width   = (uint32_t)iv.extent[0];
                    gen_params.init_image.height  = (uint32_t)iv.extent[1];
                    gen_params.init_image.channel = (uint32_t)channels;
                    gen_params.init_image.data =
                        static_cast<uint8_t*>(const_cast<void*>(iv.data));

                    // Match the source geometry, as img2img does: generating at a
                    // different size than the conditioning frame misaligns the
                    // latents.
                    gen_params.width  = iv.extent[0];
                    gen_params.height = iv.extent[1];

                    std::cerr << "[Vison] image-to-video from " << params.init_image_path
                              << " (" << iv.extent[0] << "x" << iv.extent[1] << ")" << std::endl;
                } else {
                    std::cerr << "[Vison] Ignoring init image: expected RGB or RGBA" << std::endl;
                }
            } catch (const std::exception& e) {
                result.error_message =
                    std::string("Could not decode the image to animate: ") + e.what();
                return result;
            }
        }

        struct ProgressData { const GenerateParams* params; } pdata = { &params };
        sd_set_progress_callback([](int step, int steps, float, void* data) {
            auto* p = static_cast<ProgressData*>(data);
            if (p->params->on_progress) p->params->on_progress(step, steps);
        }, &pdata);

        // Stand in for a GPU that dies here, when asked to. See
        // simulate_backend_failure_if_requested() - video is the pipeline where
        // a real device loss is most likely, since it decodes many frames.
        simulate_backend_failure_if_requested(params.vram_backoff);

        sd_image_t* frames = nullptr;
        int num_frames = 0;
        sd_audio_t* audio = nullptr;

        const bool ok = generate_video(sd_ctx_, &gen_params, &frames, &num_frames, &audio);

        if (cancelled_.load()) {
            if (frames) free_sd_images(frames, num_frames);
            if (audio) free_sd_audio(audio);
            result.error_message = "CancelledByUser";
            return result;
        }

        if (!ok || num_frames == 0 || !frames) {
            result.error_message = describe_failure("Failed to generate video");
            return result;
        }

        // Frames go to a scratch directory, get muxed, and the directory is
        // removed - so a successful run leaves exactly one playable file.
        std::string base = params.output_path.empty() ? std::string("outputs/output_vid")
                                                      : params.output_path;
        base = std::filesystem::path(base).replace_extension("").string();
        const std::string work_dir = base + "_frames";

        std::error_code ec;
        std::filesystem::create_directories(work_dir, ec);

        struct Cleanup {
            std::string dir;
            bool keep = false;
            ~Cleanup() {
                if (keep) return;
                std::error_code e;
                std::filesystem::remove_all(dir, e);
            }
        } cleanup{work_dir};

        for (int i = 0; i < num_frames; ++i) {
            char name[32];
            std::snprintf(name, sizeof(name), "/frame%05d.png", i + 1);

            visp::image_format fmt = frames[i].channel == 4 ? visp::image_format::rgba_u8
                                                            : visp::image_format::rgb_u8;
            visp::image_view view({(int)frames[i].width, (int)frames[i].height}, fmt,
                                  frames[i].data);
            save_image(view, work_dir + name, "png");
        }

        free_sd_images(frames, num_frames);
        if (audio) free_sd_audio(audio);

        // The container is the muxer's decision, not ours - it depends on which
        // encoders the available ffmpeg actually has.
        const std::string video_path = base + video_output_extension();
        std::string error;
        if (mux_frames_to_video(work_dir + "/frame%05d.png", gen_params.fps, video_path, error)) {
            result.success = true;
            result.output_path = video_path;
            std::cerr << "[Vison] Wrote " << num_frames << " frames to " << video_path
                      << " (" << video_encoder().label << ")" << std::endl;
            return result;
        }

        // No muxer available: keep the frames rather than throwing the work
        // away, and report the path that actually exists.
        cleanup.keep = true;
        std::cerr << "[Vison] Could not mux the video (" << error
                  << "); leaving " << num_frames << " PNG frames in " << work_dir << std::endl;

        char first[32];
        std::snprintf(first, sizeof(first), "/frame%05d.png", 1);
        result.success = true;
        result.output_path = work_dir + first;
        return result;
    }

    TaskType task_type() const override { return TaskType::VIDEO_GENERATION; }

private:
    sd_ctx_t* sd_ctx_ = nullptr;
    bool is_loaded_ = false;
    // These must outlive new_sd_ctx(), which keeps the pointers it is given.
    std::string backend_spec_;
    std::string params_backend_;
    std::string vram_budget_;
    BackendPlan plan_;
    std::string t5xxl_path_;
    std::string vae_path_;
    std::string high_noise_path_;
    std::string llm_path_;
    std::atomic<bool> cancelled_{false};
};

std::unique_ptr<Pipeline> create_video_gen_pipeline() {
    return std::make_unique<VideoGenerationPipeline>();
}

} // namespace vison::pipelines
