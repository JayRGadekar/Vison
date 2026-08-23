#pragma once

#include <mutex>
#include <string>

namespace vison::pipelines {

// stable-diffusion.cpp reports why a generation failed only through its log
// callback - generate_image/generate_video just return false. Without capturing
// it, a user who asks for a clip their GPU cannot fit is told "Failed to
// generate video" and has no idea that fewer frames would fix it.
//
// This records the interesting lines as they stream past so the pipeline can
// attach the real cause to the result.
class BackendErrorLog {
public:
    static BackendErrorLog& instance() {
        static BackendErrorLog log;
        return log;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        last_.clear();
    }

    // Keeps the first genuinely diagnostic line of a failure. The first is more
    // useful than the last: ggml reports the allocation that actually failed
    // and then unwinds through several generic "compute failed" messages.
    void observe(const std::string& line) {
        if (line.find("failed to allocate") == std::string::npos &&
            line.find("ErrorOutOfDeviceMemory") == std::string::npos &&
            line.find("alloc compute buffer failed") == std::string::npos &&
            line.find("Device memory allocation of size") == std::string::npos) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (last_.empty()) {
            last_ = line;
            // Trim the trailing newline the log callback includes.
            while (!last_.empty() && (last_.back() == '\n' || last_.back() == '\r')) last_.pop_back();
        }
    }

    std::string take() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string out = last_;
        last_.clear();
        return out;
    }

private:
    std::mutex mutex_;
    std::string last_;
};

}  // namespace vison::pipelines
