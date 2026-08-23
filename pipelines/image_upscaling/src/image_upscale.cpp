#include "vison/pipelines/image_upscale.h"
#include "visp/vision.h"
#include "visp/image.h"
#include "vison/pipelines/image_output.h"
#include "visp/ml.h"
#include <iostream>
#include <filesystem>
#include <algorithm>
#include <cmath>
#include <vector>
#include <cstdint>

namespace {

// The 8 symmetries of a square (the dihedral group D4), encoded in 3 bits:
//   bit 0 - mirror horizontally
//   bit 1 - mirror vertically
//   bit 2 - transpose (swap x and y, so the extent swaps too)
//
// map_forward gives the destination of a source pixel under one of them. Both
// directions of the transform are expressed through this single function so the
// forward and inverse can never drift apart.
inline void map_forward(int x, int y, int w, int h, int op, int& out_x, int& out_y) {
    const int fx = op & 1, fy = op & 2, tr = op & 4;
    const int mx = fx ? (w - 1 - x) : x;
    const int my = fy ? (h - 1 - y) : y;
    if (tr) {
        out_x = my;
        out_y = mx;
    } else {
        out_x = mx;
        out_y = my;
    }
}

inline visp::i32x2 mapped_extent(visp::i32x2 e, int op) {
    return (op & 4) ? visp::i32x2{e[1], e[0]} : e;
}

// dst[map_forward(x,y)] = src[x,y]. A bijection, so scattering leaves no holes.
visp::image_data apply_op(const visp::image_view& src, int op) {
    const int w = src.extent[0], h = src.extent[1];
    const int c = visp::n_channels(src);
    visp::image_data dst = visp::image_alloc(mapped_extent(src.extent, op), src.format);
    visp::image_view dv(dst);

    const uint8_t* sp = static_cast<const uint8_t*>(src.data);
    uint8_t* dp       = static_cast<uint8_t*>(const_cast<void*>(dv.data));

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int dx, dy;
            map_forward(x, y, w, h, op, dx, dy);
            const uint8_t* srow = sp + (size_t)y * src.stride + (size_t)x * c;
            uint8_t* drow       = dp + (size_t)dy * dv.stride + (size_t)dx * c;
            for (int k = 0; k < c; ++k) drow[k] = srow[k];
        }
    }
    return dst;
}

// Undoes apply_op on an image that has since been scaled: walks the restored
// image and pulls each pixel from where map_forward would have put it.
void accumulate_inverse(const visp::image_view& transformed, int op,
                        int w, int h, int c, std::vector<uint16_t>& acc) {
    const uint8_t* tp = static_cast<const uint8_t*>(transformed.data);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int sx, sy;
            map_forward(x, y, w, h, op, sx, sy);
            const uint8_t* src = tp + (size_t)sy * transformed.stride + (size_t)sx * c;
            uint16_t* dst      = acc.data() + ((size_t)y * w + x) * c;
            for (int k = 0; k < c; ++k) dst[k] = (uint16_t)(dst[k] + src[k]);
        }
    }
}

}  // namespace

namespace vison::pipelines {

class ImageUpscalePipeline : public Pipeline {
public:
    // The backend is chosen in load_model(), not here: allow_fallback is a
    // per-request setting and picking a device in the constructor would fix it
    // before we know what was asked for.
    ImageUpscalePipeline() = default;

    ~ImageUpscalePipeline() override {
        unload_model();
    }

    bool load_model(const std::string& gguf_path, const GenerateParams& load_params) override {
        if (!std::filesystem::exists(gguf_path)) {
            std::cerr << "Model file not found: " << gguf_path << std::endl;
            return false;
        }

        // visp::backend_init() picks the best available device. There is no way
        // to name a specific one through its API - backend_init(backend_type)
        // selects by CPU/GPU class only - which is why the model registry does
        // not advertise gpu_id for upscaling.
        try {
            backend_ = visp::backend_init();
            model_   = visp::esrgan_load_model(gguf_path.c_str(), backend_);
            is_loaded_ = true;
            return true;
        } catch (const std::exception& e) {
            std::cerr << "Failed to load upscale model '" << gguf_path << "' on the default device: "
                      << e.what() << std::endl;
            if (!load_params.allow_fallback) return false;
        }

        // ESRGAN is small enough that a CPU run is merely slow rather than
        // impractical, so this fallback is genuinely usable.
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
    
    // The ESRGAN weights have one fixed scale baked in (4x for
    // realesrgan-x4plus), so the quality the user picked in the UI has to be
    // honoured by resampling the model's output. Coming down from the 4x
    // result is exactly why "2x" here still beats a plain 2x resize of the
    // source. Returns the extent the finished image should have.
    static visp::i32x2 target_extent(const std::string& quality,
                                     visp::i32x2 source,
                                     visp::i32x2 upscaled) {
        auto fit_height = [&](int target_h) {
            const int h = std::max(1, target_h);
            const int w = std::max(1, (int)std::llround((double)source[0] * h / (double)source[1]));
            return visp::i32x2{w, h};
        };

        if (quality == "2x") return visp::i32x2{std::max(1, source[0] * 2), std::max(1, source[1] * 2)};
        if (quality == "4x") return upscaled;             // the model's native scale
        // "1080p" and friends name a vertical resolution, so match the height
        // and let the width follow the source aspect ratio.
        if (quality == "1080p") return fit_height(1080);
        if (quality == "1440p") return fit_height(1440);
        if (quality == "2160p") return fit_height(2160);
        return upscaled;
    }

    // Test-time augmentation: upscale all 8 square symmetries of the input,
    // rotate each result back, and average. Averaging 8 independently-inferred
    // estimates cancels most of the model's directional bias, which shows up as
    // steadier edges and less ringing - at 8x the runtime, so it is opt-in.
    visp::image_data esrgan_tta(const visp::image_view& input,
                                const ProgressCallback& on_progress) {
        constexpr int kOps = 8;

        visp::image_data first;      // identity pass fixes the output geometry
        std::vector<uint16_t> acc;   // 8 * 255 = 2040, so 16 bits is enough
        int ow = 0, oh = 0, oc = 0;

        for (int op = 0; op < kOps; ++op) {
            visp::image_data rotated = apply_op(input, op);
            visp::image_data up      = visp::esrgan_compute(model_, visp::image_view(rotated));
            visp::image_view uv(up);

            if (op == 0) {
                oc = visp::n_channels(uv);
                ow = uv.extent[0];
                oh = uv.extent[1];
                first = visp::image_alloc({ow, oh}, uv.format);
                acc.assign((size_t)ow * oh * oc, 0);
            }

            accumulate_inverse(uv, op, ow, oh, oc, acc);

            if (on_progress) on_progress(10 + (op + 1) * 80 / kOps, 100);
        }

        visp::image_view fv(first);
        uint8_t* out = static_cast<uint8_t*>(const_cast<void*>(fv.data));
        for (int y = 0; y < oh; ++y) {
            for (int x = 0; x < ow; ++x) {
                const uint16_t* a = acc.data() + ((size_t)y * ow + x) * oc;
                uint8_t* o        = out + (size_t)y * fv.stride + (size_t)x * oc;
                for (int k = 0; k < oc; ++k) o[k] = (uint8_t)((a[k] + kOps / 2) / kOps);
            }
        }
        return first;
    }

    GenerateResult run(const GenerateParams& params) override {
        GenerateResult result;
        result.success = false;

        if (!is_loaded_) {
            result.error_message = "Model not loaded";
            return result;
        }

        if (params.input_image_path.empty()) {
            result.error_message = "Upscaling needs a source image, but none was attached";
            return result;
        }
        if (!std::filesystem::exists(params.input_image_path)) {
            result.error_message = "Input image not found: " + params.input_image_path;
            return result;
        }

        try {
            visp::image_data input_img = visp::image_load(params.input_image_path.c_str());
            if (!input_img.data) {
                result.error_message = "Failed to load input image";
                return result;
            }

            visp::image_view input_view(input_img);
            if (params.on_progress) params.on_progress(10, 100);

            visp::image_data output_img;
            if (params.tta_mode) {
                std::cerr << "[Vison] TTA on: 8 upscale passes, expect ~8x the usual time"
                          << std::endl;
                output_img = esrgan_tta(input_view, params.on_progress);
            } else {
                output_img = visp::esrgan_compute(model_, input_view);
            }
            if (params.on_progress) params.on_progress(90, 100);

            visp::image_view output_view(output_img);
            const visp::i32x2 want =
                target_extent(params.upscale_quality, input_view.extent, output_view.extent);

            // Kept alive until image_save() has run - image_view does not own
            // its pixels.
            visp::image_data resized;
            if (want[0] != output_view.extent[0] || want[1] != output_view.extent[1]) {
                std::cerr << "[Vison] Upscale: " << input_view.extent[0] << "x" << input_view.extent[1]
                          << " -> " << output_view.extent[0] << "x" << output_view.extent[1]
                          << " (model), resampled to " << want[0] << "x" << want[1]
                          << " for quality '" << params.upscale_quality << "'" << std::endl;
                resized = visp::image_scale(output_view, want);
                output_view = visp::image_view(resized);
            }

            // Honour the destination the caller picked. The server hands every
            // task a unique outputs/ path so concurrent requests cannot clobber
            // each other; writing a fixed "output_upscaled.png" in the working
            // directory ignored that.
            std::string out_ext = params.output_format.empty() ? "png" : params.output_format;
            std::string out_path = params.output_path.empty()
                                       ? ("output_upscaled." + out_ext)
                                       : params.output_path;

            result.success = true;
            result.output_path = save_image(output_view, out_path, out_ext, params.compression);

            if (params.on_progress) params.on_progress(100, 100);
        } catch (const std::exception& e) {
            result.error_message = std::string("Exception during inference: ") + e.what();
        }

        return result;
    }

    TaskType task_type() const override { return TaskType::IMAGE_UPSCALING; }
    
private:
    visp::backend_device backend_;
    visp::esrgan_model model_;
    bool is_loaded_ = false;
};

std::unique_ptr<Pipeline> create_image_upscale_pipeline() {
    return std::make_unique<ImageUpscalePipeline>();
}

} // namespace vison::pipelines
