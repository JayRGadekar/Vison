#pragma once
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <map>
#include <cctype>
#include <cstdlib>
#include <stdexcept>

namespace vison {

enum class TaskType {
    IMAGE_GENERATION,
    IMAGE_UPSCALING,
    VIDEO_GENERATION,
    VIDEO_UPSCALING
};

inline TaskType task_from_string(const std::string& str) {
    if (str == "image_generation") return TaskType::IMAGE_GENERATION;
    if (str == "image_upscaling") return TaskType::IMAGE_UPSCALING;
    if (str == "video_generation") return TaskType::VIDEO_GENERATION;
    if (str == "video_upscaling") return TaskType::VIDEO_UPSCALING;
    return TaskType::IMAGE_GENERATION;
}

// A GPU that gets reset out from under us - the Windows TDR watchdog killing a
// submit that ran too long, a driver crash, or a shader hang - surfaces as
// VK_ERROR_DEVICE_LOST. vulkan.hpp raises that as an exception whose what() is
// "vk::Device::waitForFences: ErrorDeviceLost"; ggml's own checks and the CUDA
// backend spell it differently again. Match on the substring shared by all of
// them so callers can react regardless of who reported it.
inline bool is_device_lost_error(const std::string& message) {
    std::string lower;
    lower.reserve(message.size());
    for (char c : message) lower.push_back((char)std::tolower((unsigned char)c));
    return lower.find("devicelost") != std::string::npos ||
           lower.find("device lost") != std::string::npos ||
           lower.find("device_lost") != std::string::npos;
}

// A generation that asked for more VRAM than the card has.
//
// This must be treated exactly like a device loss, for a reason that is not
// obvious: the *failed* run leaves the cached context in a state that kills the
// process on the NEXT request. Measured on a 6GB card - a 961-frame video
// failed cleanly with ErrorOutOfDeviceMemory, and the 33-frame request that
// followed (a size that succeeds in 140s on a fresh server) aborted the backend
// with no error at all. Dropping the context after an OOM is what makes the
// failure recoverable instead of terminal.
inline bool is_out_of_memory_error(const std::string& message) {
    std::string lower;
    lower.reserve(message.size());
    for (char c : message) lower.push_back((char)std::tolower((unsigned char)c));
    return lower.find("outofdevicememory") != std::string::npos ||
           lower.find("out of device memory") != std::string::npos ||
           lower.find("failed to allocate") != std::string::npos ||
           lower.find("alloc compute buffer failed") != std::string::npos ||
           lower.find("out of memory") != std::string::npos;
}

// Fault injection for the GPU-failure recovery path.
//
// A real device loss needs the driver's watchdog to fire mid-submit, which no
// test can arrange on demand. That is how the recovery code in
// TaskQueue::worker_loop came to be trusted without ever being watched to run:
// the one time it mattered the process died first, so the retry was theory.
//
// VISON_SIMULATE_DEVICE_LOSS=N fails every attempt whose vram_backoff is below
// N as though the GPU had been reset; VISON_SIMULATE_OOM=N does the same with
// an out-of-memory failure. N=1 fails the first attempt and lets the first
// retry through - the case worth proving, because it exercises dropping the
// dead context, reloading, and succeeding. A large N proves the give-up path
// and, more importantly, that the server is still answering afterwards.
//
// The messages are the real ones verbatim, so they travel the same
// is_device_lost_error()/is_out_of_memory_error() route as a genuine failure
// rather than a special case that only exists for tests. Unset - the normal
// case - this costs two getenv calls per generation.
inline void simulate_backend_failure_if_requested(int vram_backoff) {
    if (const char* env = std::getenv("VISON_SIMULATE_DEVICE_LOSS")) {
        if (vram_backoff < std::atoi(env)) {
            throw std::runtime_error(
                "vk::Device::waitForFences: ErrorDeviceLost"
                " [simulated: VISON_SIMULATE_DEVICE_LOSS]");
        }
    }
    if (const char* env = std::getenv("VISON_SIMULATE_OOM")) {
        if (vram_backoff < std::atoi(env)) {
            throw std::runtime_error(
                "vk::Device::allocateMemory: ErrorOutOfDeviceMemory"
                " [simulated: VISON_SIMULATE_OOM]");
        }
    }
}

struct DeviceInfo {
    std::string name;                 // human-readable, e.g. "NVIDIA GeForce RTX 4050 Laptop GPU"
    size_t vram_bytes = 0;            // total device memory
    size_t free_vram_bytes = 0;       // free right now; drops while a model is loaded
    std::string backend;              // "vulkan", "cuda", "cpu", ...
    // Position within that backend's own device list. This is the number the
    // pipelines mean by "vulkan<N>", so it is what a request's gpu_id must
    // carry. -1 means the device cannot be addressed that way.
    int index = -1;
    bool integrated = false;
    std::string driver_version;
    bool cuda_available = false;
    bool vulkan_available = false;
    bool metal_available = false;
};

// Progress callback: (current_step, total_steps)
using ProgressCallback = std::function<void(int, int)>;

struct GenerateParams {
    std::string prompt;
    std::string negative_prompt;
    std::string model_path;        // Path to the model's primary weights file

    // Every other weight file the model declares, keyed by the registry's
    // "role" (clip_l, t5xxl, vae, high_noise, ...). Globbing the model
    // directory instead was fine with one model per architecture, but two Wan
    // releases ship differently-versioned VAEs sitting side by side and a glob
    // picks whichever the directory iterator happens to return first. The
    // registry already knows the answer, so it says so.
    std::map<std::string, std::string> model_files;
    TaskType task;
    int width = 512;
    int height = 512;
    int steps = 20;
    float guidance = 7.5f;
    int seed = -1;
    std::string output_format = "png";  // png, jpg, webp

    // img2img: when set, generation starts from this image instead of noise.
    // strength is how far to move away from it (0 = keep, 1 = ignore).
    std::string init_image_path;
    float strength = 0.75f;

    // Upscaling-specific
    std::string input_image_path;
    std::string upscale_quality = "4x"; // 2x, 4x, 1080p, 1440p, 2160p

    // --- Advanced settings -------------------------------------------------
    // Only a subset applies to any given task; the server's model registry
    // declares which ones each model actually supports so the UI can hide the
    // rest instead of offering controls that do nothing. Every field here is
    // wired to real behaviour - do not add one without implementing it.

    // Output encoder effort. 0 means "pipeline default".
    //   jpg: quality 1-100 (higher = larger, better)
    //   png: zlib level 1-9; values above 9 are treated as a 1-100 scale
    int compression = 0;

    // Which GPU to run on, as a backend device index ("0", "1", ...). Empty
    // means let the backend choose. Only honoured where the backend can
    // actually address a specific device.
    std::string gpu_id;

    // Fall back to CPU instead of failing when the GPU cannot be used.
    bool allow_fallback = false;

    // Tile edge in pixels for tiled work. 0 means the pipeline default.
    int tile_size = 0;

    // Test-time augmentation: run the model over 8 flip/transpose variants and
    // average them. Much better edges, roughly 8x the runtime.
    bool tta_mode = false;

    // How many times to halve the planned VRAM budget. Raised by the queue
    // after a device loss: a TDR means one submit ran too long, so repeating
    // the identical plan just reproduces it. A smaller budget cuts the graph
    // into more, shorter segments.
    int vram_backoff = 0;

    // --- Video ---------------------------------------------------------------
    // Number of frames to generate, and the rate to play them back at. Cost
    // scales with frame count far more steeply than with resolution, so the
    // defaults are deliberately short.
    int video_frames = 33;
    int fps = 16;

    // Where the finished file should be written. Empty means the pipeline
    // picks a name in the current directory (legacy behaviour).
    std::string output_path;

    ProgressCallback on_progress = nullptr;
};

struct GenerateResult {
    bool success;
    std::string output_path;       // Path to generated file
    std::string error_message;
    double elapsed_seconds;
};

// Abstract pipeline interface — each pipeline type implements this
class Pipeline {
public:
    virtual ~Pipeline() = default;
    // `params` carries the settings that are baked into the loaded context
    // rather than applied per-run - gpu_id and allow_fallback. Changing either
    // requires a reload, which is why TaskQueue folds them into its cache key.
    virtual bool load_model(const std::string& gguf_path, const GenerateParams& params) = 0;
    virtual void unload_model() = 0;
    virtual bool is_loaded() const = 0;
    virtual GenerateResult run(const GenerateParams& params) = 0;
    virtual TaskType task_type() const = 0;

    // Asks a running run() to stop early. Called from another thread while
    // run() is in progress; must be safe to call when nothing is running.
    // Default is a no-op for pipelines that cannot interrupt themselves.
    virtual void request_cancel() {}
};

// Identifies a loaded context. Two requests can share a cached pipeline only
// when this matches, because everything in it is fixed at load time.
inline std::string load_signature(const GenerateParams& p) {
    return std::to_string((int)p.task) + "|" + p.model_path + "|gpu=" + p.gpu_id +
           "|fallback=" + (p.allow_fallback ? "1" : "0") +
           "|backoff=" + std::to_string(p.vram_backoff);
}

// Factory
std::unique_ptr<Pipeline> create_pipeline(TaskType type);

// --- Backend planning --------------------------------------------------------
//
// How much of a model can live on the GPU depends entirely on the machine, so
// nothing here is hardcoded. A 6GB laptop has to keep weights in RAM and stream
// them in per graph segment; a 24GB card should hold everything resident and
// run several times faster. Committing to one fixed strategy penalises whichever
// machine it was not tuned for, so the strategy is derived per load from the
// device's real memory and the model's real size.

enum class VramTier {
    Full,       // weights and encoders resident in VRAM; nothing streamed
    Balanced,   // encoders resident, diffusion weights streamed from RAM
    Stream,     // everything in RAM, streamed per segment; encoders on CPU
    Cpu,        // no usable GPU
};

const char* to_string(VramTier tier);

// What a model costs, measured from the files themselves rather than guessed.
struct ModelFootprint {
    size_t total_bytes = 0;     // every weight file
    size_t encoder_bytes = 0;   // just the text encoders, which dominate FLUX/Wan
};

// The stable-diffusion.cpp settings to use for one load.
struct BackendPlan {
    VramTier tier = VramTier::Stream;
    std::string backend_spec;    // e.g. "all=vulkan0" or "all=vulkan0,te=cpu"
    std::string params_backend;  // empty = leave weights on the compute device
    std::string max_vram;        // empty = uncapped
    bool stream_layers = false;
    bool vae_tiling = true;
    std::string summary;         // one line, for the log
};

// Total installed system RAM, in bytes. 0 if it cannot be determined.
size_t total_system_ram();

// Whether a model can realistically run on this machine, worked out BEFORE
// anything is downloaded - discovering a 7GB download does not fit by watching
// it fail is the worst possible way to find out.
enum class Fit {
    Good,        // comfortable
    Tight,       // will run, but slowly or with little headroom
    Unsupported  // will not run on this hardware
};

struct ModelCompatibility {
    Fit fit = Fit::Good;
    VramTier tier = VramTier::Stream;
    double needs_ram_gb = 0;     // resident weights, after dequantisation
    double system_ram_gb = 0;
    double vram_gb = 0;
    std::string summary;         // one line, shown to the user
};

// `total_download_bytes` is what the registry declares for the model; resident
// cost is estimated from it the same way plan_backend() does.
ModelCompatibility check_compatibility(size_t total_download_bytes,
                                       size_t encoder_download_bytes,
                                       const std::string& gpu_id = {});

// Picks a plan for `footprint` on the requested device. `gpu_id` is a backend
// device index as reported by list_devices(); empty means device 0.
// `backoff` halves the derived budget that many times (see
// GenerateParams::vram_backoff); `budget_ceiling_gib` caps it outright. Video
// passes a lower ceiling because its graph segments carry a time dimension and
// run far longer than an image's for the same amount of memory.
BackendPlan plan_backend(const ModelFootprint& footprint, const std::string& gpu_id,
                         int backoff = 0, size_t budget_ceiling_gib = 0);

// Total/encoder bytes for a set of weight files. Missing files count as zero.
ModelFootprint measure_footprint(const std::vector<std::string>& all_paths,
                                 const std::vector<std::string>& encoder_paths);

// System
// Every GPU the loaded backends expose, in the order their backend numbers
// them, so DeviceInfo::index can be handed straight back as gpu_id.
std::vector<DeviceInfo> list_devices();

// The device that would be used if no gpu_id is given.
DeviceInfo detect_device();

} // namespace vison
