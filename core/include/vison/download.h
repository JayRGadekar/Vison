#pragma once
#include <string>
#include <cstdint>
#include <atomic>
#include <functional>

namespace vison {

// Called with (bytes_downloaded_for_this_file, total_bytes_for_this_file).
// total is 0 when the server does not report a Content-Length.
using DownloadProgressCallback = std::function<void(uint64_t, uint64_t)>;

struct DownloadResult {
    bool success = false;
    bool cancelled = false;
    uint64_t bytes = 0;
    std::string error;
};

// Downloads `url` to `dest` using WinHTTP (no Python, no external tools).
//
// Guarantees that matter for model files:
//  - writes to `dest`.part and only renames into place on success, so an
//    interrupted download can never masquerade as a complete model file
//  - resumes a previous `.part` via a Range request when the server allows it
//  - follows redirects (Hugging Face resolve/ URLs redirect to a CDN host)
//  - aborts promptly when *cancel becomes true
DownloadResult download_to_file(const std::string& url,
                                const std::string& dest,
                                DownloadProgressCallback on_progress,
                                const std::atomic<bool>* cancel);

// Returns the remote size in bytes via a HEAD request, or 0 if unknown.
uint64_t probe_remote_size(const std::string& url);

// --- Integrity verification -------------------------------------------------

// True if the file begins with the "GGUF" magic.
bool has_gguf_magic(const std::string& path);

// True if the file looks like a valid .safetensors container: an 8-byte
// little-endian header length that is sane and fits inside the file.
bool has_safetensors_header(const std::string& path);

// Verifies a downloaded model file: existence, expected size (when known and
// non-zero), and format magic inferred from the extension. `error` receives a
// human-readable reason on failure.
bool verify_model_file(const std::string& path, uint64_t expected_size, std::string& error);

// --- Connectivity diagnostics ----------------------------------------------

struct ProbeResult {
    bool ok = false;
    int status = 0;
    bool got_byte = false;      // whether an actual body byte could be read
    double seconds = 0.0;
    std::string error;
};

// Issues a GET and tries to read one body byte. Reading a byte (rather than
// just checking the status) is what distinguishes "the API answers" from
// "large file transfers actually work" on networks that reset mid-download.
ProbeResult probe_endpoint(const std::string& url);

} // namespace vison
