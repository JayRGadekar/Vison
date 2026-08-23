#include "vison/vison.h"

#include "ggml-backend.h"

#ifdef _WIN32
// windows.h defines min/max as macros, which then swallow std::min/std::max
// used below. NOMINMAX has to come before the include to suppress them.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <cctype>
#include <cstring>
#include <cstdio>
#include <vector>
#include <filesystem>
#include <algorithm>
#include <mutex>
#include <string>

namespace vison {
namespace {

// ggml loads its backend DLLs at runtime, so nothing is enumerable until this
// has run. It is idempotent but not cheap, hence once_flag rather than a call
// on every query.
void ensure_backends_loaded() {
    static std::once_flag once;
    std::call_once(once, [] { ggml_backend_load_all(); });
}

bool reg_is(ggml_backend_dev_t dev, const char* name) {
    ggml_backend_reg_t reg = ggml_backend_dev_backend_reg(dev);
    if (reg == nullptr) return false;
    const char* reg_name = ggml_backend_reg_name(reg);
    return reg_name != nullptr && std::strcmp(reg_name, name) == 0;
}

}  // namespace

std::vector<DeviceInfo> list_devices() {
    ensure_backends_loaded();

    std::vector<DeviceInfo> devices;
    // `backend_index` counts per backend, not globally: the image generation
    // pipeline addresses GPUs as "vulkan0", "vulkan1", ... and that numbering
    // is the position within the Vulkan backend's own device list. Numbering
    // across all backends would make gpu_id point at the wrong card.
    int vulkan_index = 0;
    int cuda_index   = 0;

    const size_t count = ggml_backend_dev_count();
    for (size_t i = 0; i < count; ++i) {
        ggml_backend_dev_t dev = ggml_backend_dev_get(i);
        if (dev == nullptr) continue;

        // `enum` is required: ggml_backend_dev_type names both the enum and
        // the accessor function, and the function shadows the type here.
        const enum ggml_backend_dev_type dev_type = ggml_backend_dev_type(dev);
        if (dev_type == GGML_BACKEND_DEVICE_TYPE_CPU) continue;  // not selectable as a gpu_id

        DeviceInfo info;
        const char* desc = ggml_backend_dev_description(dev);
        info.name = (desc != nullptr && *desc) ? desc : "Unknown device";

        size_t free_bytes = 0, total_bytes = 0;
        ggml_backend_dev_memory(dev, &free_bytes, &total_bytes);
        info.vram_bytes = total_bytes;
        info.free_vram_bytes = free_bytes;

        if (reg_is(dev, "Vulkan")) {
            info.backend = "vulkan";
            info.index   = vulkan_index++;
            info.vulkan_available = true;
        } else if (reg_is(dev, "CUDA")) {
            info.backend = "cuda";
            info.index   = cuda_index++;
            info.cuda_available = true;
        } else {
            ggml_backend_reg_t reg = ggml_backend_dev_backend_reg(dev);
            const char* reg_name = reg ? ggml_backend_reg_name(reg) : nullptr;
            info.backend = reg_name ? reg_name : "unknown";
            info.index   = -1;   // not addressable through our backend spec
        }

        info.integrated = (dev_type == GGML_BACKEND_DEVICE_TYPE_IGPU);
        devices.push_back(std::move(info));
    }

    return devices;
}

DeviceInfo detect_device() {
    // "The device we would use by default", which is the first GPU the active
    // backend reports - the same one the pipelines get from "vulkan0".
    auto devices = list_devices();

    for (const auto& d : devices) {
        if (d.index == 0) return d;
    }
    if (!devices.empty()) return devices.front();

    DeviceInfo cpu_only;
    cpu_only.name    = "CPU";
    cpu_only.backend = "cpu";
    cpu_only.index   = -1;
    return cpu_only;
}

}  // namespace vison

namespace vison {

const char* to_string(VramTier tier) {
    switch (tier) {
        case VramTier::Full:     return "full-vram";
        case VramTier::Balanced: return "balanced";
        case VramTier::Stream:   return "streaming";
        case VramTier::Cpu:      return "cpu";
    }
    return "unknown";
}

ModelFootprint measure_footprint(const std::vector<std::string>& all_paths,
                                 const std::vector<std::string>& encoder_paths) {
    ModelFootprint fp;
    std::error_code ec;
    for (const auto& p : all_paths) {
        if (p.empty()) continue;
        auto sz = std::filesystem::file_size(p, ec);
        if (!ec) fp.total_bytes += (size_t)sz;
    }
    for (const auto& p : encoder_paths) {
        if (p.empty()) continue;
        auto sz = std::filesystem::file_size(p, ec);
        if (!ec) fp.encoder_bytes += (size_t)sz;
    }
    return fp;
}

BackendPlan plan_backend(const ModelFootprint& footprint, const std::string& gpu_id,
                         int backoff, size_t budget_ceiling_gib) {
    constexpr size_t GiB = 1024ull * 1024 * 1024;

    // Quantised weights expand when they are dequantised for compute - FLUX is
    // 12.2GB of files but 15.9GB resident, and Wan 7.2GB -> 10.1GB. 1.35x
    // matches both closely enough to plan with.
    const size_t resident_total   = (size_t)(footprint.total_bytes * 1.35);
    const size_t resident_encoder = (size_t)(footprint.encoder_bytes * 1.35);

    // Room for activations on top of weights. The largest single compute buffer
    // seen here is Wan's VAE decode at 3.1GB, so anything below that has to keep
    // VAE tiling on.
    constexpr size_t compute_reserve = 3ull * GiB;

    BackendPlan plan;

    // A user who names a device gets that device; otherwise plan for the one
    // the pipelines would use anyway.
    std::string index = gpu_id.empty() ? "0" : gpu_id;

    auto devices = list_devices();
    const DeviceInfo* dev = nullptr;
    for (const auto& d : devices) {
        if (d.index >= 0 && std::to_string(d.index) == index && d.backend == "vulkan") {
            dev = &d;
            break;
        }
    }
    if (dev == nullptr && !devices.empty()) dev = &devices.front();

    if (dev == nullptr) {
        plan.tier = VramTier::Cpu;
        plan.backend_spec = "all=cpu";
        plan.vae_tiling = false;   // no GPU watchdog to worry about
        plan.summary = "no GPU detected; running on CPU";
        return plan;
    }

    // Plan against free rather than total memory: a desktop compositor, a
    // browser, or another model can already be holding a chunk of the card.
    // Keep a slice back so we are not the reason something else fails.
    const size_t free_vram = dev->free_vram_bytes > 0 ? dev->free_vram_bytes : dev->vram_bytes;
    size_t budget          = (size_t)(free_vram * 0.85);

    // Fitting is only half the problem. A larger budget means fewer, larger
    // graph segments, which means longer individual submits - and Windows
    // resets the GPU when one runs past ~2s. Measured on this card: a 9-frame
    // video at a 4 GiB budget lost the device twice in a row, while the same
    // job at 2 GiB finished in 156s. So callers whose submits are long (video)
    // cap the budget, and a device loss halves it again on the retry.
    if (budget_ceiling_gib > 0) budget = std::min(budget, budget_ceiling_gib * GiB);
    for (int i = 0; i < backoff && budget > GiB; ++i) budget /= 2;

    const std::string spec_device = "vulkan" + index;

    // An explicit profile wins over the measurement: useful for a user who
    // knows their setup better than a heuristic can, and the only way to
    // exercise a tier the current hardware cannot reach.
    std::string forced;
    if (const char* env = std::getenv("VISON_VRAM_PROFILE")) {
        for (const char* c = env; *c; ++c) forced.push_back((char)std::tolower((unsigned char)*c));
    }
    if (forced == "auto") forced.clear();

    if (forced == "full" || (forced.empty() && budget >= resident_total + compute_reserve)) {
        // Everything fits with room to compute. No offload, no streaming, no
        // segmenting - this is where a large card earns its money, and capping
        // it here is exactly the "tuned for someone else's laptop" mistake.
        plan.tier           = VramTier::Full;
        plan.backend_spec   = "all=" + spec_device;
        plan.params_backend = "";
        plan.max_vram       = "";
        plan.stream_layers  = false;
        plan.vae_tiling     = false;
    } else if (forced == "balanced" ||
               (forced.empty() && budget >= resident_encoder + compute_reserve)) {
        // The encoders fit and stay resident, which removes the slowest part of
        // the streaming path; the diffusion weights still come from RAM.
        plan.tier           = VramTier::Balanced;
        plan.backend_spec   = "all=" + spec_device;
        plan.params_backend = "*=cpu";
        plan.max_vram       = std::to_string(std::max<size_t>(1, budget / GiB));
        plan.stream_layers  = true;
        plan.vae_tiling     = true;
    } else {
        // Small card: keep the encoders on the CPU entirely. A resident T5-XXL
        // would take the whole budget and leave nothing to compute with.
        plan.tier           = VramTier::Stream;
        plan.backend_spec   = "all=" + spec_device + ",te=cpu";
        plan.params_backend = "*=cpu";
        plan.max_vram       = std::to_string(std::max<size_t>(1, budget / GiB));
        plan.stream_layers  = true;
        plan.vae_tiling     = true;
    }

    if (forced == "cpu") {
        plan.tier = VramTier::Cpu;
        plan.backend_spec = "all=cpu";
        plan.params_backend.clear();
        plan.max_vram.clear();
        plan.stream_layers = false;
        plan.vae_tiling = false;
        // Describe it as what it is. The generic summary below reads an empty
        // params_backend as "resident in VRAM", which is nonsense for a CPU
        // plan and named the GPU it was pointedly not using.
        plan.summary = "cpu (forced): running entirely on the processor, no GPU used";
        return plan;
    }

    const double free_gb  = (double)free_vram / (double)GiB;
    const double model_gb = (double)resident_total / (double)GiB;
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "%s on %s (%.1f GiB free, model needs ~%.1f GiB resident): backend='%s'%s%s%s",
                  to_string(plan.tier), dev->name.c_str(), free_gb, model_gb,
                  plan.backend_spec.c_str(),
                  plan.params_backend.empty() ? ", weights in VRAM" : ", weights in RAM",
                  plan.max_vram.empty() ? ", uncapped" : (", max_vram=" + plan.max_vram + " GiB").c_str(),
                  plan.stream_layers ? ", layer streaming" : "");
    plan.summary = buf;

    return plan;
}

size_t total_system_ram() {
#ifdef _WIN32
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) return (size_t)status.ullTotalPhys;
    return 0;
#else
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long page_size = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && page_size > 0) return (size_t)pages * (size_t)page_size;
    return 0;
#endif
}

ModelCompatibility check_compatibility(size_t total_download_bytes,
                                       size_t encoder_download_bytes,
                                       const std::string& gpu_id) {
    constexpr size_t GiB = 1024ull * 1024 * 1024;
    ModelCompatibility out;

    // Same 1.35x dequantisation factor plan_backend() uses, calibrated on FLUX
    // (12.2GB of files -> 15.9GB resident) and Wan (7.2 -> 10.1).
    const size_t resident = (size_t)(total_download_bytes * 1.35);
    out.needs_ram_gb = (double)resident / (double)GiB;

    const size_t ram = total_system_ram();
    out.system_ram_gb = (double)ram / (double)GiB;

    ModelFootprint fp;
    fp.total_bytes   = total_download_bytes;
    fp.encoder_bytes = encoder_download_bytes;
    const BackendPlan plan = plan_backend(fp, gpu_id);
    out.tier = plan.tier;

    auto devices = list_devices();
    if (!devices.empty()) out.vram_gb = (double)devices.front().vram_bytes / (double)GiB;

    // The wall is RAM, not VRAM: weights live in system memory
    // (params_backend="*=cpu") and only the compute buffer needs the card.
    //
    // But compare against the FILE size, not the dequantised size. enable_mmap
    // maps the weights from disk, so the OS holds a working set rather than
    // every tensor at once - measured, FLUX is 12.2GB of files and 15.9GB of
    // "params memory" yet runs in 9.7GB of private memory. An earlier version
    // of this check compared the 15.9GB figure against 15.2GB of RAM and
    // declared FLUX unsupported, which is plainly wrong: it generates a
    // 1024x576 image here in 87s.
    const double files_gb = (double)total_download_bytes / (double)GiB;

    // Small models round to "0.0 GB", which reads like a bug rather than like
    // "tiny". Switch to MB below a gigabyte.
    auto size_str = [](double gb) {
        char b[32];
        if (gb < 1.0) std::snprintf(b, sizeof(b), "%.0f MB", gb * 1024.0);
        else          std::snprintf(b, sizeof(b), "%.1f GB", gb);
        return std::string(b);
    };

    char cbuf[400];
    if (ram > 0 && files_gb > out.system_ram_gb) {
        out.fit = Fit::Unsupported;
        std::snprintf(cbuf, sizeof(cbuf),
                      "Needs %s of weights but this machine has %.1f GB of RAM. It would "
                      "page to disk continuously and is unlikely to finish.",
                      size_str(files_gb).c_str(), out.system_ram_gb);
    } else if (ram > 0 && files_gb > out.system_ram_gb * 0.70) {
        out.fit = Fit::Tight;
        std::snprintf(cbuf, sizeof(cbuf),
                      "%s of weights against %.1f GB of RAM. It runs, but memory is tight "
                      "and generation is slow.",
                      size_str(files_gb).c_str(), out.system_ram_gb);
    } else if (plan.tier == VramTier::Stream) {
        out.fit = Fit::Tight;
        std::snprintf(cbuf, sizeof(cbuf),
                      "Runs with weights streamed from RAM (%s) because %.1f GB of VRAM "
                      "cannot hold them. Expect slower generation.",
                      size_str(out.needs_ram_gb).c_str(), out.vram_gb);
    } else {
        out.fit = Fit::Good;
        std::snprintf(cbuf, sizeof(cbuf),
                      "Fits comfortably: %s of weights, %.1f GB VRAM, %.1f GB RAM.",
                      size_str(out.needs_ram_gb).c_str(), out.vram_gb, out.system_ram_gb);
    }
    out.summary = cbuf;
    return out;
}

}  // namespace vison
