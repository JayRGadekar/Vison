#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace vison::pipelines {

namespace detail {
// Directory holding the running executable. A packaged app keeps its helper
// binaries next to the server, and that location is not on PATH and is not any
// of the well-known install prefixes - so without this, a bundled ffmpeg would
// sit right beside us and never be found.
inline std::filesystem::path exe_dir() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return std::filesystem::path(std::wstring(buf, n)).parent_path();
#else
    std::error_code ec;
    auto self = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::filesystem::path{} : self.parent_path();
#endif
}
}  // namespace detail


// Video containers need a muxer, and neither stable-diffusion.cpp nor
// vision.cpp ships one. Rather than vendor a codec, shell out to ffmpeg when it
// is available and degrade to a PNG frame sequence when it is not - so video
// works properly on a machine that has ffmpeg, and still produces something
// usable on one that does not.

// Locates ffmpeg once. Empty string means "not available".
inline const std::string& ffmpeg_path() {
    static const std::string path = [] {
        // An explicit override wins, so a bundled copy can be pointed at.
        if (const char* env = std::getenv("VISON_FFMPEG")) {
            if (*env && std::filesystem::exists(env)) return std::string(env);
        }
        // Beside the executable first: that is where a bundled copy lives, and
        // a copy we shipped should always beat whatever the machine happens to
        // have installed.
        const auto here = detail::exe_dir();
        if (!here.empty()) {
#ifdef _WIN32
            const char* names[] = {"ffmpeg.exe", "bin/ffmpeg.exe", "ffmpeg/ffmpeg.exe"};
#else
            const char* names[] = {"ffmpeg", "bin/ffmpeg", "ffmpeg/ffmpeg"};
#endif
            for (const char* n : names) {
                std::error_code ec;
                const auto p = here / n;
                if (std::filesystem::exists(p, ec)) return p.string();
            }
        }

        const char* candidates[] = {
            "C:/ffmpeg/ffmpeg.exe",
            "C:/ffmpeg/bin/ffmpeg.exe",
            "C:/Program Files/ffmpeg/bin/ffmpeg.exe",
            "/usr/bin/ffmpeg",
            "/usr/local/bin/ffmpeg",
            "/opt/homebrew/bin/ffmpeg",
        };
        for (const char* c : candidates) {
            std::error_code ec;
            if (std::filesystem::exists(c, ec)) return std::string(c);
        }
        // Last resort: let the OS resolve it from PATH. Verified below by
        // actually running it, since existence cannot be tested this way.
#ifdef _WIN32
        if (std::system("where ffmpeg >nul 2>&1") == 0) return std::string("ffmpeg");
#else
        if (std::system("command -v ffmpeg >/dev/null 2>&1") == 0) return std::string("ffmpeg");
#endif
        return std::string();
    }();
    return path;
}

inline bool ffmpeg_available() { return !ffmpeg_path().empty(); }

// A path is pasted into a shell command line, so anything that could terminate
// the quoting is rejected outright rather than escaped. Our own paths never
// contain these; a request that smuggles one in gets a clean refusal.
//
// '%' is deliberately NOT in that set: ffmpeg's frame patterns need exactly one
// ("frame%05d.png"). cmd.exe only expands a '%' when it is paired around a name
// ("%PATH%"), so a single one is inert while a second is rejected.
inline bool path_is_shell_safe(const std::string& p) {
    if (p.find_first_of("\"'`$&|;<>^!\n\r") != std::string::npos) return false;
    return std::count(p.begin(), p.end(), '%') <= 1;
}

namespace detail {

inline int run_quiet(const std::string& command) {
#ifdef _WIN32
    // cmd.exe strips the outer quote pair when the command starts with one, so
    // the whole thing needs wrapping for a quoted exe path to survive.
    const std::string wrapped = "\"" + command + "\" >nul 2>&1";
#else
    const std::string wrapped = command + " >/dev/null 2>&1";
#endif
    return std::system(wrapped.c_str());
}

inline std::string quote(const std::string& s) { return "\"" + s + "\""; }

}  // namespace detail

// Which encoder and container to write.
//
// VP9 in WebM, not H.264 in mp4. Two reasons, both licensing rather than
// technical: libx264 is GPL, so an ffmpeg built to use it can only be shipped
// under the GPL, and H.264 additionally sits in the Via LA patent pool, which
// is a live question for a paid product that distributes an encoder. libvpx is
// BSD-licensed and VP9 is royalty-free by design, so a WebM build can simply be
// bundled. Chromium - which is what the Electron window is - plays VP9 natively.
//
// The cost is portability: .webm is less convenient than .mp4 if someone drags
// the file into another editor. That was a deliberate trade.
struct VideoEncoderChoice {
    std::string args;        // encoder + tuning flags
    std::string extension;   // including the dot
    std::string label;       // for the log
};

namespace detail {

// Captures a command's stdout. Needed because encoder support varies between
// ffmpeg builds and guessing wrong means every video fails at the last step.
inline std::string capture(const std::string& command) {
#ifdef _WIN32
    const std::string wrapped = "\"" + command + " 2>&1\"";
    FILE* pipe = _popen(wrapped.c_str(), "r");
#else
    FILE* pipe = popen((command + " 2>&1").c_str(), "r");
#endif
    if (!pipe) return {};
    std::string out;
    char buf[4096];
    while (std::fgets(buf, sizeof(buf), pipe)) out += buf;
#ifdef _WIN32
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    return out;
}

}  // namespace detail

inline const VideoEncoderChoice& video_encoder() {
    static const VideoEncoderChoice choice = [] {
        const std::string encoders =
            ffmpeg_available() ? detail::capture(detail::quote(ffmpeg_path()) +
                                                 " -hide_banner -encoders") : std::string();
        auto has = [&encoders](const char* name) {
            return encoders.find(name) != std::string::npos;
        };

        // -b:v 0 with -crf is VP9's constant-quality mode; without the explicit
        // zero bitrate libvpx silently switches to a constrained-quality mode
        // and the result looks far worse than the crf suggests. row-mt and
        // cpu-used matter a lot: libvpx-vp9 at its defaults is slow enough that
        // encoding would become a visible share of generation time.
        if (has("libvpx-vp9")) {
            return VideoEncoderChoice{
                "-c:v libvpx-vp9 -pix_fmt yuv420p -b:v 0 -crf 32 -row-mt 1 -cpu-used 4 -deadline good",
                ".webm", "VP9/WebM"};
        }
        if (has("libvpx")) {
            return VideoEncoderChoice{
                "-c:v libvpx -pix_fmt yuv420p -b:v 2M -cpu-used 4 -deadline good",
                ".webm", "VP8/WebM"};
        }
        // Native, always present, and not GPL. Worse compression, but it means
        // an ffmpeg without libvpx still produces a playable file rather than
        // failing at the very end of a long generation.
        return VideoEncoderChoice{
            "-c:v mpeg4 -pix_fmt yuv420p -q:v 3", ".mp4", "MPEG-4 Part 2 (no libvpx in this ffmpeg)"};
    }();
    return choice;
}

// The extension the muxer will produce. Callers build their output path from
// this rather than assuming a container.
inline const std::string& video_output_extension() { return video_encoder().extension; }

// Muxes a printf-style numbered frame sequence (e.g. ".../frame%03d.png") into
// a video the Electron window can play directly.
inline bool mux_frames_to_video(const std::string& frame_pattern,
                                int fps,
                                const std::string& out_path,
                                std::string& error) {
    if (!ffmpeg_available()) {
        error = "ffmpeg not found";
        return false;
    }
    if (!path_is_shell_safe(frame_pattern) || !path_is_shell_safe(out_path)) {
        error = "refusing to run ffmpeg on a path containing shell metacharacters";
        return false;
    }

    // The even-dimension scale filter stays regardless of codec: yuv420p halves
    // the chroma planes, so an odd width or height has nowhere to put the last
    // row of samples.
    const std::string cmd =
        detail::quote(ffmpeg_path()) + " -y -framerate " + std::to_string(fps <= 0 ? 16 : fps) +
        " -i " + detail::quote(frame_pattern) + " " +
        video_encoder().args +
        " -vf \"scale=trunc(iw/2)*2:trunc(ih/2)*2\" " +
        detail::quote(out_path);

    if (detail::run_quiet(cmd) != 0) {
        error = "ffmpeg failed to mux frames into " + out_path;
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::exists(out_path, ec)) {
        error = "ffmpeg reported success but produced no file";
        return false;
    }
    return true;
}

// Writes a raw float waveform (as produced by sd_audio_t: sample_count
// interleaved frames of `channels` 32-bit floats each) as a standard
// IEEE-float WAV file. ffmpeg reads WAV natively, so this is the simplest
// correct intermediate container to hand it - no need to vendor an encoder
// just to get audio from a float buffer into ffmpeg's -i.
inline bool write_wav_float(const float* data, uint64_t sample_count,
                            uint32_t sample_rate, uint32_t channels,
                            const std::string& out_path, std::string& error) {
    if (!data || sample_count == 0 || channels == 0 || sample_rate == 0) {
        error = "empty or invalid audio buffer";
        return false;
    }

    std::ofstream f(out_path, std::ios::binary);
    if (!f) {
        error = "could not open " + out_path + " for writing";
        return false;
    }

    const uint32_t bits_per_sample = 32;
    const uint32_t byte_rate = sample_rate * channels * (bits_per_sample / 8);
    const uint16_t block_align = static_cast<uint16_t>(channels * (bits_per_sample / 8));
    const uint32_t data_bytes = static_cast<uint32_t>(sample_count * channels * (bits_per_sample / 8));
    const uint32_t riff_size = 36 + data_bytes;

    auto write_u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto write_u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };

    f.write("RIFF", 4);
    write_u32(riff_size);
    f.write("WAVE", 4);
    f.write("fmt ", 4);
    write_u32(16);          // fmt chunk size
    write_u16(3);            // WAVE_FORMAT_IEEE_FLOAT
    write_u16(static_cast<uint16_t>(channels));
    write_u32(sample_rate);
    write_u32(byte_rate);
    write_u16(block_align);
    write_u16(static_cast<uint16_t>(bits_per_sample));
    f.write("data", 4);
    write_u32(data_bytes);
    f.write(reinterpret_cast<const char*>(data), data_bytes);

    if (!f.good()) {
        error = "failed writing WAV data to " + out_path;
        return false;
    }
    return true;
}

// The audio encoder that pairs with whichever container video_encoder()
// picked. Opus for WebM matches the video encoder's licensing stance (BSD,
// royalty-free); AAC for the mpeg4 fallback container, which is already the
// GPL/patent-avoidant path's worse option, so a widely-supported but
// licensed codec there doesn't add a new constraint beyond what that
// fallback already accepts. Empty means "mux video without audio".
inline const std::string& audio_encoder_args() {
    static const std::string args = [] {
        const std::string encoders =
            ffmpeg_available() ? detail::capture(detail::quote(ffmpeg_path()) +
                                                 " -hide_banner -encoders") : std::string();
        auto has = [&encoders](const char* name) {
            return encoders.find(name) != std::string::npos;
        };
        if (video_output_extension() == ".webm") {
            if (has("libopus")) return std::string("-c:a libopus -b:a 128k");
            if (has("libvorbis")) return std::string("-c:a libvorbis -q:a 4");
            return std::string();
        }
        if (has("aac")) return std::string("-c:a aac -b:a 128k");
        return std::string();
    }();
    return args;
}

// Same as mux_frames_to_video, but also mixes in a WAV audio track (e.g. one
// written by write_wav_float). `-shortest` trims to the shorter of the two
// streams, since a decoder-produced audio tail can run a few samples past or
// short of the frame count.
inline bool mux_frames_and_audio_to_video(const std::string& frame_pattern,
                                          int fps,
                                          const std::string& audio_path,
                                          const std::string& out_path,
                                          std::string& error) {
    if (!ffmpeg_available()) {
        error = "ffmpeg not found";
        return false;
    }
    if (!path_is_shell_safe(frame_pattern) || !path_is_shell_safe(out_path) ||
        !path_is_shell_safe(audio_path)) {
        error = "refusing to run ffmpeg on a path containing shell metacharacters";
        return false;
    }
    const std::string& audio_args = audio_encoder_args();
    if (audio_args.empty()) {
        error = "no compatible audio encoder available in this ffmpeg build";
        return false;
    }

    const std::string cmd =
        detail::quote(ffmpeg_path()) + " -y -framerate " + std::to_string(fps <= 0 ? 16 : fps) +
        " -i " + detail::quote(frame_pattern) +
        " -i " + detail::quote(audio_path) + " " +
        video_encoder().args + " " + audio_args +
        " -vf \"scale=trunc(iw/2)*2:trunc(ih/2)*2\" -shortest " +
        detail::quote(out_path);

    if (detail::run_quiet(cmd) != 0) {
        error = "ffmpeg failed to mux frames and audio into " + out_path;
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::exists(out_path, ec)) {
        error = "ffmpeg reported success but produced no file";
        return false;
    }
    return true;
}

// Explodes a video into numbered PNGs in `out_dir`, returning their paths in
// order. Empty on failure.
inline std::vector<std::string> extract_video_frames(const std::string& video_path,
                                                     const std::string& out_dir,
                                                     std::string& error) {
    std::vector<std::string> frames;
    if (!ffmpeg_available()) {
        error = "ffmpeg not found";
        return frames;
    }
    if (!path_is_shell_safe(video_path) || !path_is_shell_safe(out_dir)) {
        error = "refusing to run ffmpeg on a path containing shell metacharacters";
        return frames;
    }

    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);

    const std::string pattern = out_dir + "/frame%05d.png";
    const std::string cmd = detail::quote(ffmpeg_path()) + " -y -i " + detail::quote(video_path) +
                            " " + detail::quote(pattern);
    if (detail::run_quiet(cmd) != 0) {
        error = "ffmpeg could not decode " + video_path;
        return frames;
    }

    for (int i = 1;; ++i) {
        char name[32];
        std::snprintf(name, sizeof(name), "frame%05d.png", i);
        std::string p = out_dir + "/" + name;
        if (!std::filesystem::exists(p, ec)) break;
        frames.push_back(std::move(p));
    }
    if (frames.empty()) error = "no frames were decoded from " + video_path;
    return frames;
}

// Frame rate of a video, or `fallback` when it cannot be determined. Keeping
// the source rate is what stops an upscale from silently changing playback
// speed.
inline double probe_video_fps(const std::string& video_path, double fallback = 16.0) {
    if (!ffmpeg_available() || !path_is_shell_safe(video_path)) return fallback;

    const std::string cmd = detail::quote(ffmpeg_path()) + " -i " + detail::quote(video_path) +
                            " -hide_banner 2>&1";
#ifdef _WIN32
    FILE* pipe = _popen(("\"" + cmd + "\"").c_str(), "r");
#else
    FILE* pipe = popen(cmd.c_str(), "r");
#endif
    if (!pipe) return fallback;

    std::string out;
    char buf[512];
    while (std::fgets(buf, sizeof(buf), pipe)) out += buf;
#ifdef _WIN32
    _pclose(pipe);
#else
    pclose(pipe);
#endif

    // ffmpeg prints "..., 30 fps, ..." in the stream description line.
    auto pos = out.find(" fps");
    if (pos == std::string::npos || pos == 0) return fallback;
    size_t end = pos;
    size_t start = out.find_last_of(" ,", end - 1);
    if (start == std::string::npos) return fallback;
    try {
        double v = std::stod(out.substr(start + 1, end - start - 1));
        if (v > 0.0 && v < 1000.0) return v;
    } catch (...) {
    }
    return fallback;
}

}  // namespace vison::pipelines
