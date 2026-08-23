#include "vison/download.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <vector>
#include <cstdio>
#include <chrono>

namespace vison {
namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), n);
    return out;
}

std::string last_error_message(const char* what) {
    DWORD code = GetLastError();
    char buf[512];
    std::snprintf(buf, sizeof(buf), "%s failed (WinHTTP error %lu)", what, (unsigned long)code);
    return std::string(buf);
}

// RAII wrapper so every early return closes its handle.
struct WinHttpHandle {
    HINTERNET h = nullptr;
    WinHttpHandle() = default;
    explicit WinHttpHandle(HINTERNET handle) : h(handle) {}
    ~WinHttpHandle() { if (h) WinHttpCloseHandle(h); }
    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;
    operator HINTERNET() const { return h; }
    explicit operator bool() const { return h != nullptr; }
};

struct ParsedUrl {
    bool ok = false;
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = 0;
    bool https = false;
};

ParsedUrl parse_url(const std::string& url) {
    ParsedUrl out;
    std::wstring wurl = widen(url);

    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[512]{};
    wchar_t path[4096]{};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 512;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 4096;

    if (!WinHttpCrackUrl(wurl.c_str(), (DWORD)wurl.size(), 0, &uc)) return out;

    out.host = host;
    out.path = path;
    out.port = uc.nPort;
    out.https = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    out.ok = true;
    return out;
}

const wchar_t* kUserAgent = L"Vison/0.1 (WinHTTP)";

// Opens a session with timeouts sized for multi-GB transfers.
HINTERNET open_session() {
    HINTERNET session = WinHttpOpen(kUserAgent,
                                    WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS,
                                    0);
    if (!session) return nullptr;
    // resolve, connect, send, receive (milliseconds)
    WinHttpSetTimeouts(session, 30000, 30000, 60000, 60000);
    return session;
}

uint64_t content_length_of(HINTERNET request) {
    wchar_t len_buf[64]{};
    DWORD len_size = sizeof(len_buf);
    if (WinHttpQueryHeaders(request,
                            WINHTTP_QUERY_CONTENT_LENGTH,
                            WINHTTP_HEADER_NAME_BY_INDEX,
                            len_buf, &len_size, WINHTTP_NO_HEADER_INDEX)) {
        return (uint64_t)_wtoi64(len_buf);
    }
    return 0;
}

DWORD status_code_of(HINTERNET request) {
    DWORD status = 0;
    DWORD size = sizeof(status);
    WinHttpQueryHeaders(request,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    return status;
}

} // namespace

uint64_t probe_remote_size(const std::string& url) {
    ParsedUrl u = parse_url(url);
    if (!u.ok) return 0;

    WinHttpHandle session(open_session());
    if (!session) return 0;
    WinHttpHandle connect(WinHttpConnect(session, u.host.c_str(), u.port, 0));
    if (!connect) return 0;

    DWORD flags = u.https ? WINHTTP_FLAG_SECURE : 0;
    WinHttpHandle request(WinHttpOpenRequest(connect, L"HEAD", u.path.c_str(),
                                             nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (!request) return 0;

    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) return 0;
    if (!WinHttpReceiveResponse(request, nullptr)) return 0;

    DWORD status = status_code_of(request);
    if (status < 200 || status >= 300) return 0;
    return content_length_of(request);
}

DownloadResult download_to_file(const std::string& url,
                                const std::string& dest,
                                DownloadProgressCallback on_progress,
                                const std::atomic<bool>* cancel) {
    DownloadResult result;
    const std::string part = dest + ".part";

    ParsedUrl u = parse_url(url);
    if (!u.ok) {
        result.error = "Malformed URL: " + url;
        return result;
    }

    std::error_code ec;
    auto parent = std::filesystem::path(dest).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);

    // Resume from an existing .part if one is present.
    uint64_t already = 0;
    if (std::filesystem::exists(part, ec)) {
        already = (uint64_t)std::filesystem::file_size(part, ec);
        if (ec) already = 0;
    }

    WinHttpHandle session(open_session());
    if (!session) { result.error = last_error_message("WinHttpOpen"); return result; }

    WinHttpHandle connect(WinHttpConnect(session, u.host.c_str(), u.port, 0));
    if (!connect) { result.error = last_error_message("WinHttpConnect"); return result; }

    DWORD flags = u.https ? WINHTTP_FLAG_SECURE : 0;
    WinHttpHandle request(WinHttpOpenRequest(connect, L"GET", u.path.c_str(),
                                             nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (!request) { result.error = last_error_message("WinHttpOpenRequest"); return result; }

    std::wstring range_header;
    if (already > 0) {
        range_header = L"Range: bytes=" + std::to_wstring(already) + L"-";
    }

    if (!WinHttpSendRequest(request,
                            range_header.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : range_header.c_str(),
                            range_header.empty() ? 0 : (DWORD)-1L,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        result.error = last_error_message("WinHttpSendRequest");
        return result;
    }

    if (!WinHttpReceiveResponse(request, nullptr)) {
        result.error = last_error_message("WinHttpReceiveResponse");
        return result;
    }

    DWORD status = status_code_of(request);
    if (status == 416) {
        // Requested range unsatisfiable: the .part is >= the remote file.
        // Discard it so a retry starts clean.
        std::filesystem::remove(part, ec);
        result.error = "Stale partial download discarded; retry the download.";
        return result;
    }
    if (status < 200 || status >= 300) {
        result.error = "HTTP " + std::to_string(status) + " for " + url;
        return result;
    }

    // 206 means the Range was honoured and we append. A 200 means the server
    // sent the whole file regardless, so we must restart from byte zero.
    const bool resumed = (status == 206 && already > 0);
    if (!resumed) already = 0;

    const uint64_t body_bytes = content_length_of(request);
    const uint64_t total = body_bytes ? body_bytes + already : 0;

    std::ofstream out(part, std::ios::binary | (resumed ? std::ios::app : std::ios::trunc));
    if (!out) {
        result.error = "Cannot open for writing: " + part;
        return result;
    }

    uint64_t downloaded = already;
    std::vector<char> buffer(1024 * 256);
    if (on_progress) on_progress(downloaded, total);

    // Report at most every ~4 MB so a multi-GB file does not spam the callback.
    uint64_t last_reported = downloaded;
    const uint64_t report_interval = 4ull * 1024 * 1024;

    for (;;) {
        if (cancel && cancel->load()) {
            out.close();
            result.cancelled = true;
            result.error = "Cancelled by user";
            // Keep the .part file so the transfer can resume later.
            return result;
        }

        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            out.close();
            result.error = last_error_message("WinHttpQueryDataAvailable");
            return result;
        }
        if (available == 0) break;

        DWORD to_read = (DWORD)(std::min)((size_t)available, buffer.size());
        DWORD read = 0;
        if (!WinHttpReadData(request, buffer.data(), to_read, &read)) {
            out.close();
            result.error = last_error_message("WinHttpReadData");
            return result;
        }
        if (read == 0) break;

        out.write(buffer.data(), (std::streamsize)read);
        if (!out) {
            out.close();
            result.error = "Write failed (disk full?): " + part;
            return result;
        }
        downloaded += read;

        if (on_progress && (downloaded - last_reported >= report_interval)) {
            last_reported = downloaded;
            on_progress(downloaded, total);
        }
    }

    out.close();
    if (on_progress) on_progress(downloaded, total);

    // Refuse to publish a short file. This is the check that stops a truncated
    // transfer from being mistaken for a usable model.
    if (total > 0 && downloaded < total) {
        result.error = "Incomplete download: got " + std::to_string(downloaded) +
                       " of " + std::to_string(total) + " bytes";
        return result;
    }

    std::filesystem::remove(dest, ec);
    std::filesystem::rename(part, dest, ec);
    if (ec) {
        result.error = "Could not move " + part + " into place: " + ec.message();
        return result;
    }

    result.success = true;
    result.bytes = downloaded;
    return result;
}

// --- Connectivity diagnostics ----------------------------------------------

ProbeResult probe_endpoint(const std::string& url) {
    ProbeResult out;
    auto started = std::chrono::steady_clock::now();

    auto finish = [&]() {
        out.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        return out;
    };

    ParsedUrl u = parse_url(url);
    if (!u.ok) {
        out.error = "Malformed URL";
        return finish();
    }

    WinHttpHandle session(open_session());
    if (!session) { out.error = last_error_message("WinHttpOpen"); return finish(); }

    WinHttpHandle connect(WinHttpConnect(session, u.host.c_str(), u.port, 0));
    if (!connect) { out.error = last_error_message("WinHttpConnect"); return finish(); }

    DWORD flags = u.https ? WINHTTP_FLAG_SECURE : 0;
    WinHttpHandle request(WinHttpOpenRequest(connect, L"GET", u.path.c_str(),
                                             nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (!request) { out.error = last_error_message("WinHttpOpenRequest"); return finish(); }

    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        out.error = last_error_message("WinHttpSendRequest");
        return finish();
    }
    if (!WinHttpReceiveResponse(request, nullptr)) {
        out.error = last_error_message("WinHttpReceiveResponse");
        return finish();
    }

    out.status = (int)status_code_of(request);
    out.ok = (out.status >= 200 && out.status < 400);

    // Try to pull a single body byte — a status alone does not prove that bulk
    // transfer works on this network.
    char byte = 0;
    DWORD read = 0;
    if (WinHttpReadData(request, &byte, 1, &read) && read == 1) {
        out.got_byte = true;
    }

    return finish();
}

// --- Integrity verification -------------------------------------------------

bool has_gguf_magic(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    char magic[4] = {};
    f.read(magic, 4);
    if (f.gcount() != 4) return false;
    return magic[0] == 'G' && magic[1] == 'G' && magic[2] == 'U' && magic[3] == 'F';
}

bool has_safetensors_header(const std::string& path) {
    std::error_code ec;
    auto size = (uint64_t)std::filesystem::file_size(path, ec);
    if (ec || size < 8) return false;

    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    unsigned char raw[8] = {};
    f.read(reinterpret_cast<char*>(raw), 8);
    if (f.gcount() != 8) return false;

    uint64_t header_len = 0;
    for (int i = 7; i >= 0; --i) header_len = (header_len << 8) | raw[i];

    // The JSON header must be non-empty and fit inside the file after the
    // 8-byte length prefix.
    if (header_len == 0 || header_len + 8 > size) return false;

    // First non-whitespace byte of the header must open a JSON object.
    char c = 0;
    while (f.get(c)) {
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
        return c == '{';
    }
    return false;
}

bool verify_model_file(const std::string& path, uint64_t expected_size, std::string& error) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        error = "missing";
        return false;
    }

    auto actual = (uint64_t)std::filesystem::file_size(path, ec);
    if (ec) {
        error = "unreadable";
        return false;
    }

    if (expected_size > 0 && actual != expected_size) {
        error = "size mismatch (expected " + std::to_string(expected_size) +
                " bytes, found " + std::to_string(actual) + ")";
        return false;
    }

    auto ext = std::filesystem::path(path).extension().string();
    if (ext == ".gguf") {
        if (!has_gguf_magic(path)) {
            error = "not a valid GGUF file (bad magic)";
            return false;
        }
    } else if (ext == ".safetensors") {
        if (!has_safetensors_header(path)) {
            error = "not a valid safetensors file (bad header)";
            return false;
        }
    } else if (actual < 1024) {
        error = "implausibly small (" + std::to_string(actual) + " bytes)";
        return false;
    }

    error.clear();
    return true;
}

} // namespace vison
