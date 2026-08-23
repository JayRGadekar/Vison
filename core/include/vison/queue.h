#pragma once
#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <atomic>
#include <thread>
#include <string>
#include <memory>

#include "vison/vison.h"

namespace vison {

struct Task {
    std::string id;
    GenerateParams params;
    std::function<void(const GenerateResult&)> on_complete;
};

class TaskQueue {
public:
    TaskQueue();
    ~TaskQueue();

    std::string enqueue(const GenerateParams& params,
                        std::function<void(const GenerateResult&)> on_complete);

    // Drops a task that is still queued. Returns true if it was found.
    bool cancel(const std::string& task_id);

    // Asks the currently running task to stop as soon as possible, and drops
    // everything still queued. Returns true if something was actually running.
    bool cancel_current();

    size_t pending_count() const;

    // Releases any cached model so its memory goes back to the OS.
    void unload_cached_model();

private:
    void worker_loop();

    // Loads/reuses the pipeline for a task and runs it once, converting any
    // exception into a failed GenerateResult.
    GenerateResult run_once(const Task& task);

    // Releases the cached pipeline; used to discard a dead GPU context.
    void drop_cached_pipeline();

    std::queue<Task> queue_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> running_{true};
    std::thread worker_thread_;

    // --- Model cache -------------------------------------------------------
    // Loading FLUX costs ~30s and ~16GB of reads, so the pipeline is kept alive
    // between requests and only rebuilt when the task type or model file
    // actually changes. Owned by the worker thread; cached_mutex_ guards the
    // pointer itself so cancel_current() can reach it from an HTTP thread.
    std::unique_ptr<Pipeline> cached_pipeline_;
    TaskType cached_task_type_{};
    std::string cached_model_path_;
    // load_signature() of the request that built cached_pipeline_. Compared
    // instead of the path alone so a change to gpu_id or allow_fallback forces
    // a reload rather than silently reusing a context built for other hardware.
    std::string cached_load_signature_;

    mutable std::mutex cached_mutex_;
    Pipeline* active_pipeline_ = nullptr;   // non-null only while run() executes
    std::atomic<bool> cancel_requested_{false};
};

} // namespace vison
