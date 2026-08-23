#include "vison/pipelines/image_gen.h"
#include "stable-diffusion.h"
#include "visp/image.h" // Reuse vision.cpp image loading
#include "vison/pipelines/image_output.h"
#include "vison/pipelines/backend_error.h"
#include <iostream>
#include <filesystem>
#include <chrono>
#include <atomic>
#include <cstdlib>
#include <string>
#include <algorithm>

namespace vison::pipelines {

class ImageGenerationPipeline : public Pipeline {
public:
    ImageGenerationPipeline() {}

    // gpu_id reaches us as free text from an HTTP request and is pasted into a
    // backend spec string, so only accept a plain device index. Anything else
    // falls back to device 0 rather than producing a spec sd.cpp cannot parse.
    static std::string sanitize_device_index(const std::string& gpu_id) {
        if (gpu_id.empty()) return "0";
        if (gpu_id.size() > 2 ||
            gpu_id.find_first_not_of("0123456789") != std::string::npos) {
            std::cerr << "[Vison] Ignoring unusable gpu_id '" << gpu_id
                      << "'; expected a device index like \"0\". Using device 0."
                      << std::endl;
            return "0";
        }
        return gpu_id;
    }

    ~ImageGenerationPipeline() override {
        unload_model();
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

        std::string vae_path_str = "";
        std::string clip_l_path_str = "";
        std::string t5xxl_path_str = "";
        std::string llm_path_str = "";

        // What a model needs alongside its transformer is a property of the
        // model, and the registry already states it per file. Read it from
        // there rather than guessing from the path.
        //
        // This used to sniff the filename for "flux" and hardcode the FLUX
        // trio, which was wrong in both directions: it missed every other split
        // model - Z-Image and Qwen-Image condition on an LLM instead of CLIP+T5
        // and would have been loaded as single-file checkpoints - and it
        // misfired on any single-file checkpoint whose path happened to contain
        // the word, including a user's own "flux-style-mix.gguf".
        auto declared = [&](const char* role) -> std::string {
            auto it = load_params.model_files.find(role);
            if (it == load_params.model_files.end()) return {};
            std::error_code ec;
            return std::filesystem::exists(it->second, ec) ? it->second : std::string{};
        };

        vae_path_str    = declared("vae");
        clip_l_path_str = declared("clip_l");
        t5xxl_path_str  = declared("t5xxl");
        llm_path_str    = declared("llm");

        // Declaring any companion at all means this file is a bare diffusion
        // transformer; declaring none means it is a self-contained checkpoint
        // like SDXL Turbo, which carries its own encoders and VAE.
        const bool is_split = !vae_path_str.empty() || !clip_l_path_str.empty() ||
                              !t5xxl_path_str.empty() || !llm_path_str.empty();

        if (is_split) {
            // Deliberately NO "look for companions beside the model" fallback.
            //
            // Every model dir is flat and shared by every model, so "beside"
            // means "belonging to some other model". Z-Image declares only an
            // llm and a vae, and a probe for conventional filenames handed it
            // FLUX's clip_l.safetensors and t5xxl_fp8_e4m3fn.safetensors
            // because those happened to be in the same folder - 5.1 GB of
            // encoders it does not use, pushing the resident estimate from
            // ~10 GiB to 16.6 GiB and changing the backend plan accordingly.
            //
            // Every model that reaches a pipeline comes from a registry entry
            // whose files are all declared (see /api/generate in server.cpp),
            // so there is nothing legitimate for such a fallback to find.

            // A VAE is the one part nothing can substitute: without it there is
            // no way back from latents to pixels. Text conditioning can come
            // from either CLIP+T5 or an LLM depending on the architecture, so
            // require one of the two rather than a specific pair.
            if (vae_path_str.empty()) {
                std::cerr << "[Vison] ERROR: " << gguf_path
                          << " declares companion files but no vae." << std::endl;
                return false;
            }
            if (t5xxl_path_str.empty() && clip_l_path_str.empty() && llm_path_str.empty()) {
                std::cerr << "[Vison] ERROR: no text encoder declared for " << gguf_path
                          << " (expected clip_l/t5xxl, or llm)." << std::endl;
                return false;
            }

            if (!vae_path_str.empty())    params.vae_path    = vae_path_str.c_str();
            if (!clip_l_path_str.empty()) params.clip_l_path = clip_l_path_str.c_str();
            if (!t5xxl_path_str.empty())  params.t5xxl_path  = t5xxl_path_str.c_str();
            if (!llm_path_str.empty())    params.llm_path    = llm_path_str.c_str();

            // A split .gguf (e.g. city96's FLUX) contains ONLY the diffusion
            // transformer, not a full checkpoint. It must be passed as
            // diffusion_model_path with model_path left empty, otherwise
            // stable-diffusion.cpp parses it as a complete checkpoint, binds
            // none of the transformer tensors, and fails metadata validation
            // with "flux: depth = 0, depth_single_blocks = 0".
            params.diffusion_model_path = gguf_path.c_str();
            params.model_path = "";
            std::cerr << "[Vison] Split load: diffusion=" << gguf_path
                      << (clip_l_path_str.empty() ? "" : (" clip_l=" + clip_l_path_str))
                      << (t5xxl_path_str.empty()  ? "" : (" t5xxl="  + t5xxl_path_str))
                      << (llm_path_str.empty()    ? "" : (" llm="    + llm_path_str))
                      << " vae=" << vae_path_str << std::endl;
        } else {
            // Single-file checkpoint (e.g. SDXL Turbo) carries its own encoders + VAE.
            params.model_path = gguf_path.c_str();
        }
        
        // Read weights straight out of the model files instead of copying them
        // into the heap. stable-diffusion.cpp defaults this off, and on a
        // machine where the weights do not comfortably fit in RAM that default
        // is the difference between running and thrashing: FLUX's params total
        // ~15.9GB, so on a 16GB box the tail of them spills to the pagefile and
        // every streamed segment has to fault it back in. That shows up as
        // sustained disk I/O, a pegged CPU, and a GPU sitting idle between
        // segments waiting to be fed.
        //
        // Mapped pages are file-backed, so the OS can drop them under pressure
        // and re-read them from the original .gguf/.safetensors instead of
        // writing them to the pagefile first.
        //
        // VISON_NO_MMAP=1 restores the old copy-into-RAM behaviour, which is
        // worth having on a machine with plenty of RAM and a slow disk.
        params.enable_mmap = std::getenv("VISON_NO_MMAP") == nullptr;

        // NOT auto_fit. It sounds like a safety net (it lets a VAE decode retry
        // with tiling on OOM), but it also runs derive_backend_specs() at load
        // time, which *overwrites* the explicit backend assignment below. On a
        // 6GB card it decides the diffusion model will not fit and reassigns it
        // to the CPU, so the whole transformer silently runs on the processor:
        // "flux compute buffer size: 463.07 MB(RAM)" instead of "(VRAM)", with
        // the GPU parked at idle clocks. The VAE tiling configured per-request
        // in run() already covers the case auto_fit was wanted for.

        // Explicitly select the GPU backend the build was configured for.
        // Without this, sd_get_default_backend() may silently fall back to CPU
        // if the backend's dynamic library fails to load at runtime.
#if defined(VISON_CUDA)
        static const char* selected_backend = "cuda";
        params.flash_attn = true;
        params.diffusion_flash_attn = true;
        std::cerr << "[Vison] CUDA backend requested" << std::endl;
        params.backend = selected_backend;
#elif defined(VISON_VULKAN)
        static const char* selected_backend = "vulkan";
        params.flash_attn = true;
        params.diffusion_flash_attn = true;

        // How the model is placed is decided per load from the actual device,
        // not baked in. A 6GB laptop has to keep weights in RAM and stream them
        // per graph segment; a 24GB card should hold the whole stack resident
        // and skip all of that. Hardcoding either choice makes the app slow on
        // hardware it was not tuned for, so plan_backend() measures the files
        // and the free VRAM and picks.
        //
        // Explicit env settings still win, so a user can force anything:
        //   VISON_VRAM_PROFILE=full|balanced|stream|cpu   (whole strategy)
        //   VISON_BACKEND_SPEC=all=vulkan0                (device placement)
        //   VISON_MAX_VRAM=8                              (compute budget)
        const ModelFootprint footprint = measure_footprint(
            {gguf_path, vae_path_str, clip_l_path_str, t5xxl_path_str, llm_path_str},
            {clip_l_path_str, t5xxl_path_str, llm_path_str});

        // No ceiling for stills: a 4 GiB budget has been stable across repeated
        // 1024x576 and 1024x1024 runs here. The backoff still applies if a
        // device loss proves otherwise on other hardware.
        plan_ = plan_backend(footprint, sanitize_device_index(load_params.gpu_id),
                             load_params.vram_backoff);

        if (const char* env = std::getenv("VISON_BACKEND_SPEC")) plan_.backend_spec = env;
        if (const char* env = std::getenv("VISON_MAX_VRAM")) plan_.max_vram = env;

        backend_spec_    = plan_.backend_spec;
        params_backend_  = plan_.params_backend;
        vram_budget_     = plan_.max_vram;

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
        (void)selected_backend;
#else
        std::cerr << "[Vison] Using default (CPU) backend" << std::endl;
#endif
        
        sd_set_log_callback([](enum sd_log_level_t level, const char* text, void* data) {
            std::cerr << text;
            if (text) BackendErrorLog::instance().observe(text);
        }, nullptr);
        
        sd_ctx_ = new_sd_ctx(&params);

        // allow_fallback: a GPU that cannot host the model at all (no Vulkan
        // device, driver refuses, not enough VRAM even to start) fails here
        // rather than mid-run. Rebuild on the CPU instead of returning an
        // error, but say so loudly - FLUX on a CPU is minutes per step, not
        // seconds, and a user who did not read the setting will assume a hang.
        if (sd_ctx_ == nullptr && load_params.allow_fallback) {
            std::cerr << "[Vison] GPU context could not be created and allow_fallback is on; "
                         "retrying on the CPU. Expect this to be DRASTICALLY slower."
                      << std::endl;
            backend_spec_ = "all=cpu";
            params.backend = backend_spec_.c_str();
            params.params_backend = nullptr;   // nothing to offload; weights are already in RAM
            params.max_vram = nullptr;         // graph-cut segmenting is a VRAM concept
            params.stream_layers = false;
            sd_ctx_ = new_sd_ctx(&params);
            if (sd_ctx_ != nullptr) {
                using_cpu_fallback_ = true;
                std::cerr << "[Vison] Running on CPU (fallback)" << std::endl;
            }
        }

        if (sd_ctx_ == nullptr) {
            std::cerr << "Failed to create sd_ctx for model: " << gguf_path << std::endl;
#ifdef VISON_CUDA
            std::cerr << "Hint: CUDA was enabled at compile time. Ensure CUDA runtime DLLs are available and your GPU has enough VRAM." << std::endl;
#endif
            return false;
        }
        
        is_loaded_ = true;
        return true;
    }
    
    void unload_model() override {
        if (!is_loaded_ || !sd_ctx_) return;

        // Clear our handle first: if the teardown below fails part way through
        // we must not be left holding a pointer we might free a second time.
        sd_ctx_t* ctx = sd_ctx_;
        sd_ctx_ = nullptr;
        is_loaded_ = false;

        // Freeing a context submits and waits on the GPU one last time, so on a
        // device that has just been reset it throws (vulkan.hpp raises on every
        // non-success VkResult). This runs from ~ImageGenerationPipeline, and a
        // destructor is noexcept, so letting that escape would call
        // std::terminate and take the whole server down instead of merely
        // losing the model. Leaking the dead context is the better trade.
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
    
    GenerateResult run(const GenerateParams& params) override {
        GenerateResult result;
        result.success = false;
        
        if (!is_loaded_ || !sd_ctx_) {
            result.error_message = "Model not loaded";
            return result;
        }
        
        // Clear any cancellation left over from a previous run, otherwise this
        // generation would abort immediately.
        sd_cancel_generation(sd_ctx_, SD_CANCEL_RESET);
        cancelled_ = false;
        BackendErrorLog::instance().clear();

        sd_img_gen_params_t gen_params;
        sd_img_gen_params_init(&gen_params);
        gen_params.prompt = params.prompt.c_str();
        gen_params.negative_prompt = params.negative_prompt.c_str();
        gen_params.width = params.width;
        gen_params.height = params.height;
        gen_params.sample_params.sample_steps = params.steps;
        gen_params.sample_params.guidance.txt_cfg = params.guidance;
        gen_params.seed = params.seed;

#if defined(VISON_VULKAN) || defined(VISON_CUDA)
        // Decode the VAE in tiles on GPU backends.
        //
        // Sampling is many small per-step submits, but VAE decode is ONE huge
        // convolution over the full image. At 1024x576 that single submit runs
        // long enough to trip the Windows GPU watchdog (TDR), which kills the
        // device: sampling would report "completed, taking 82s" and then the
        // decode died with vk::Device::waitForFences: ErrorDeviceLost.
        // Tiling splits the decode into ~256px tiles, so each submit is short.
        // Leaving the tile sizes at 0 selects the built-in 32-latent default.
        // Tiling is protection against a single oversized submit; a card with
        // room to spare does not need it and is faster without.
        gen_params.vae_tiling_params.enabled = plan_.vae_tiling && !using_cpu_fallback_;
        gen_params.vae_tiling_params.target_overlap = 0.25f;

        // tile_size arrives in pixels because that is what the UI shows, but
        // sd.cpp sizes VAE tiles in LATENTS - 8 pixels each for every VAE we
        // load. Leaving the fields at 0 selects its built-in 32-latent (256px)
        // default; get_tile_sizes() clamps anything below 4 latents.
        if (params.tile_size > 0) {
            const int latent_tile = std::max(4, params.tile_size / 8);
            gen_params.vae_tiling_params.tile_size_x = latent_tile;
            gen_params.vae_tiling_params.tile_size_y = latent_tile;
            std::cerr << "[Vison] VAE tiling at " << latent_tile << " latents ("
                      << latent_tile * 8 << "px) from tile_size=" << params.tile_size
                      << std::endl;
        }
#endif

        // img2img: start from the supplied image instead of pure noise.
        // visp owns the decoded pixels, so it must outlive generate_image().
        visp::image_data init_img;
        if (!params.init_image_path.empty()) {
            if (!std::filesystem::exists(params.init_image_path)) {
                result.error_message = "Init image not found: " + params.init_image_path;
                return result;
            }
            try {
                init_img = visp::image_load(params.init_image_path.c_str());
            } catch (const std::exception& e) {
                result.error_message = std::string("Could not decode init image: ") + e.what();
                return result;
            }

            visp::image_view init_view(init_img);
            const int channels = visp::n_channels(init_view);
            if (channels != 3 && channels != 4) {
                result.error_message = "Init image must be RGB or RGBA";
                return result;
            }

            gen_params.init_image.width = (uint32_t)init_view.extent[0];
            gen_params.init_image.height = (uint32_t)init_view.extent[1];
            gen_params.init_image.channel = (uint32_t)channels;
            gen_params.init_image.data = static_cast<uint8_t*>(const_cast<void*>(init_view.data));
            gen_params.strength = params.strength;

            // Generating at a different size than the source would misalign the
            // latents, so follow the input image's dimensions.
            gen_params.width = init_view.extent[0];
            gen_params.height = init_view.extent[1];

            std::cerr << "[Vison] img2img from " << params.init_image_path << " ("
                      << init_view.extent[0] << "x" << init_view.extent[1]
                      << ", strength=" << params.strength << ")" << std::endl;
        }

        // Setup progress callback
        // We can use a global or thread-local to pass progress back, 
        // since the C API takes a void* data pointer.
        struct ProgressData {
            const GenerateParams* params;
        } pdata = { &params };
        
        sd_set_progress_callback([](int step, int steps, float time, void* data) {
            auto* p = static_cast<ProgressData*>(data);
            if (p->params->on_progress) {
                p->params->on_progress(step, steps);
            }
        }, &pdata);
        sd_image_t* images = nullptr;
        int num_images = 0;
        bool gen_success = generate_image(sd_ctx_, &gen_params, &images, &num_images);

        if (cancelled_.load()) {
            if (images) free_sd_images(images, num_images);
            result.error_message = "CancelledByUser";
            return result;
        }

        if (!gen_success || !images || num_images == 0) {
            result.error_message = describe_failure("Failed to generate image");
            return result;
        }

        sd_image_t* img = &images[0];

        // Save using vision.cpp's image_save
        visp::image_format fmt = img->channel == 4 ? visp::image_format::rgba_u8 : visp::image_format::rgb_u8;
        visp::image_view view({(int)img->width, (int)img->height}, fmt, img->data);

        std::string out_ext = params.output_format.empty() ? "png" : params.output_format;
        std::string out_path = params.output_path.empty()
                                   ? ("output_gen." + out_ext)
                                   : params.output_path;

        // save_image() encodes to the requested container and hands back the
        // path it really wrote, which differs from out_path when the format had
        // to fall back to PNG.
        std::string written = save_image(view, out_path, out_ext, params.compression);
        free_sd_images(images, num_images);

        result.success = true;
        result.output_path = written;

        return result;
    }

    TaskType task_type() const override { return TaskType::IMAGE_GENERATION; }

    void request_cancel() override {
        if (sd_ctx_) {
            cancelled_ = true;
            // SD_CANCEL_ALL unwinds the sampler at the next step boundary rather
            // than waiting for the whole image to finish.
            sd_cancel_generation(sd_ctx_, SD_CANCEL_ALL);
            std::cerr << "[Vison] Cancellation requested" << std::endl;
        }
    }

private:
    sd_ctx_t* sd_ctx_ = nullptr;
    bool is_loaded_ = false;
    // These must outlive new_sd_ctx(), which keeps the pointers it is handed.
    std::string backend_spec_;
    std::string params_backend_;
    std::string vram_budget_;
    BackendPlan plan_;
    bool using_cpu_fallback_ = false;
    std::atomic<bool> cancelled_{false};
};

std::unique_ptr<Pipeline> create_image_gen_pipeline() {
    return std::make_unique<ImageGenerationPipeline>();
}

} // namespace vison::pipelines
