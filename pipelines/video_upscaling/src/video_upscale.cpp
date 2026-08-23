#include "vison/pipelines/video_upscale.h"
#include "visp/vision.h"
#include "visp/image.h"
#include "vison/pipelines/image_output.h"
#include "vison/pipelines/video_io.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>

namespace vison::pipelines {

class VideoUpscalePipeline : public Pipeline {
public:
    // Backend selection waits for load_model() so allow_fallback can influence
    // it; picking a device in the constructor would decide before we know what
    // was asked for.
    VideoUpscalePipeline() = default;

    ~VideoUpscalePipeline() override {
        unload_model();
    }

    bool load_model(const std::string& gguf_path, const GenerateParams& load_params) override {
        if (!std::filesystem::exists(gguf_path)) {
            std::cerr << "Model file not found: " << gguf_path << std::endl;
            return false;
        }

        try {
            backend_ = visp::backend_init();
            model_   = visp::esrgan_load_model(gguf_path.c_str(), backend_);
            is_loaded_ = true;
            return true;
        } catch (const std::exception& e) {
            std::cerr << "Failed to load upscale model '" << gguf_path
                      << "' on the default device: " << e.what() << std::endl;
            if (!load_params.allow_fallback) return false;
        }

        try {
            std::cerr << "[Vison] allow_fallback is on; retrying the upscale model on the CPU"
                      << std::endl;
            backend_ = visp::backend_init(visp::backend_type::cpu);
            model_   = visp::esrgan_load_model(gguf_path.c_str(), backend_);
            is_loaded_ = true;
            std::cerr << "[Vison] Upscaling on CPU (fallback)" << std::endl;
            return true;
        } catch (const std::exception& e) {
            std::cerr << "Failed to load upscale model '" << gguf_path << "' on the CPU too: "
                      << e.what() << std::endl;
            return false;
        }
    }

    void unload_model() override {
        if (!is_loaded_) return;
        is_loaded_ = false;

        // Releasing the model tears down its ggml backend buffers, which throws
        // on a GPU that has been reset. Called from a destructor, so it must
        // never let an exception escape.
        try {
            model_ = visp::esrgan_model{};
        } catch (const std::exception& e) {
            std::cerr << "[Vison] Ignoring error while releasing the upscale model: "
                      << e.what() << std::endl;
        } catch (...) {
            std::cerr << "[Vison] Ignoring unknown error while releasing the upscale model"
                      << std::endl;
        }
    }

    bool is_loaded() const override {
        return is_loaded_;
    }

    void request_cancel() override { cancelled_ = true; }

    GenerateResult run(const GenerateParams& params) override {
        GenerateResult result;
        result.success = false;
        cancelled_ = false;

        if (!is_loaded_) {
            result.error_message = "Model not loaded";
            return result;
        }
        if (params.input_image_path.empty()) {
            result.error_message = "Video upscaling needs a source video, but none was attached";
            return result;
        }
        if (!std::filesystem::exists(params.input_image_path)) {
            result.error_message = "Input video not found: " + params.input_image_path;
            return result;
        }
        if (!ffmpeg_available()) {
            result.error_message =
                "Video upscaling needs ffmpeg to decode and re-encode the video, and none was "
                "found. Install ffmpeg (or point VISON_FFMPEG at it) and try again.";
            return result;
        }

        // Everything lands in a per-request scratch directory so two concurrent
        // jobs cannot read each other's frames, and so cleanup is one remove_all.
        const std::string work_dir =
            (std::filesystem::path(params.output_path.empty() ? std::string("outputs/vid")
                                                              : params.output_path)
                 .parent_path() /
             ("_vidup_" + std::to_string(
                              std::chrono::steady_clock::now().time_since_epoch().count())))
                .string();
        const std::string in_dir  = work_dir + "/in";
        const std::string out_dir = work_dir + "/out";

        std::error_code ec;
        std::filesystem::create_directories(out_dir, ec);

        struct Cleanup {
            std::string dir;
            ~Cleanup() {
                std::error_code e;
                std::filesystem::remove_all(dir, e);
            }
        } cleanup{work_dir};

        std::string error;
        const double fps = probe_video_fps(params.input_image_path);
        auto frames = extract_video_frames(params.input_image_path, in_dir, error);
        if (frames.empty()) {
            result.error_message = "Could not read the video: " + error;
            return result;
        }

        std::cerr << "[Vison] Upscaling " << frames.size() << " frames at " << fps << " fps"
                  << std::endl;

        try {
            for (size_t i = 0; i < frames.size(); ++i) {
                if (cancelled_.load()) {
                    result.error_message = "CancelledByUser";
                    return result;
                }

                visp::image_data src = visp::image_load(frames[i].c_str());
                if (!src.data) {
                    result.error_message = "Failed to decode frame " + frames[i];
                    return result;
                }

                visp::image_view sv(src);
                visp::image_data up = visp::esrgan_compute(model_, sv);
                visp::image_view uv(up);

                // Match the still-image pipeline's handling of upscale_quality
                // so a 2x/1080p choice means the same thing for video.
                const visp::i32x2 want =
                    target_extent(params.upscale_quality, sv.extent, uv.extent);
                visp::image_data resized;
                if (want[0] != uv.extent[0] || want[1] != uv.extent[1]) {
                    resized = visp::image_scale(uv, want);
                    uv = visp::image_view(resized);
                }

                char name[32];
                std::snprintf(name, sizeof(name), "/frame%05d.png", (int)i + 1);
                // PNG for the intermediate frames regardless of the requested
                // container: this is the encoder's input, not the deliverable,
                // and re-compressing it lossily here would show up in the video.
                save_image(uv, out_dir + name, "png");

                if (params.on_progress) {
                    params.on_progress((int)((i + 1) * 95 / frames.size()), 100);
                }
            }
        } catch (const std::exception& e) {
            result.error_message = std::string("Exception during inference: ") + e.what();
            return result;
        }

        std::string out_path = params.output_path.empty()
                                   ? std::string("outputs/output_upscaled") + video_output_extension()
                                   : params.output_path;
        // The caller's extension follows the still-image formats (png/jpg);
        // what comes out of here is a video, so name it accordingly.
        out_path = std::filesystem::path(out_path)
                       .replace_extension(video_output_extension()).string();

        if (!mux_frames_to_video(out_dir + "/frame%05d.png", (int)std::lround(fps), out_path,
                                 error)) {
            result.error_message = "Upscaled every frame but could not re-encode the video: " + error;
            return result;
        }

        if (params.on_progress) params.on_progress(100, 100);
        result.success = true;
        result.output_path = out_path;
        return result;
    }

    TaskType task_type() const override { return TaskType::VIDEO_UPSCALING; }

private:
    // Same rule the still-image upscaler uses: the model has one fixed scale,
    // so the requested quality is honoured by resampling its output.
    static visp::i32x2 target_extent(const std::string& quality,
                                     visp::i32x2 source,
                                     visp::i32x2 upscaled) {
        auto fit_height = [&](int target_h) {
            const int h = std::max(1, target_h);
            const int w = std::max(1, (int)std::llround((double)source[0] * h / (double)source[1]));
            return visp::i32x2{w, h};
        };
        if (quality == "2x") return visp::i32x2{std::max(1, source[0] * 2), std::max(1, source[1] * 2)};
        if (quality == "4x") return upscaled;
        if (quality == "1080p") return fit_height(1080);
        if (quality == "1440p") return fit_height(1440);
        if (quality == "2160p") return fit_height(2160);
        return upscaled;
    }

    visp::backend_device backend_;
    visp::esrgan_model model_;
    bool is_loaded_ = false;
    std::atomic<bool> cancelled_{false};
};

std::unique_ptr<Pipeline> create_video_upscale_pipeline() {
    return std::make_unique<VideoUpscalePipeline>();
}

} // namespace vison::pipelines
