#include "vison/queue.h"
#include <iostream>
#include <filesystem>
#include <chrono>

namespace vison {

TaskQueue::TaskQueue() {
    worker_thread_ = std::thread(&TaskQueue::worker_loop, this);
}

TaskQueue::~TaskQueue() {
    running_ = false;
    cv_.notify_all();
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
}

std::string TaskQueue::enqueue(const GenerateParams& params,
                               std::function<void(const GenerateResult&)> on_complete) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Generate simple ID
    auto now = std::chrono::system_clock::now().time_since_epoch();
    std::string id = "task_" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());

    queue_.push({id, params, std::move(on_complete)});
    cv_.notify_one();

    return id;
}

bool TaskQueue::cancel(const std::string& task_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Rebuild the queue without the requested task. std::queue has no erase,
    // so this is the straightforward way to drop one entry.
    std::queue<Task> kept;
    bool found = false;
    while (!queue_.empty()) {
        Task t = std::move(queue_.front());
        queue_.pop();
        if (!found && t.id == task_id) {
            found = true;
            if (t.on_complete) t.on_complete({false, "", "Cancelled before it started", 0});
            continue;
        }
        kept.push(std::move(t));
    }
    queue_ = std::move(kept);
    return found;
}

bool TaskQueue::cancel_current() {
    // Drop anything still waiting so a cancel does not just start the next job.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        while (!queue_.empty()) {
            Task t = std::move(queue_.front());
            queue_.pop();
            if (t.on_complete) t.on_complete({false, "", "Cancelled before it started", 0});
        }
    }

    std::lock_guard<std::mutex> lock(cached_mutex_);
    if (!active_pipeline_) return false;

    cancel_requested_ = true;
    active_pipeline_->request_cancel();
    return true;
}

void TaskQueue::unload_cached_model() {
    std::lock_guard<std::mutex> lock(cached_mutex_);
    if (active_pipeline_) return;  // never yank a model out from under a running job
    cached_pipeline_.reset();
    cached_model_path_.clear();
    cached_load_signature_.clear();
}

size_t TaskQueue::pending_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

// Loads (or reuses) the pipeline a task needs and runs it once. Any failure -
// including a C++ exception thrown from deep inside ggml - comes back as a
// GenerateResult with success == false rather than propagating, so the caller
// can decide whether the failure is worth retrying.
GenerateResult TaskQueue::run_once(const Task& task) {
    GenerateResult result;
    result.success = false;

    // Reuse the loaded model when the request targets the same pipeline
    // and file. This is what turns a repeat FLUX generation from ~30s of
    // model loading plus sampling into sampling alone.
    //
    // The key is the full load signature, not just the path: gpu_id and
    // allow_fallback are baked into the context when it is built, so a request
    // that changes either has to get a freshly loaded one.
    const std::string want_signature = load_signature(task.params);

    bool reusable;
    {
        std::lock_guard<std::mutex> lock(cached_mutex_);
        reusable = cached_pipeline_ && cached_pipeline_->is_loaded() &&
                   cached_task_type_ == task.params.task &&
                   cached_load_signature_ == want_signature;
    }

    if (!reusable) {
        {
            // Free the previous model before allocating the next one —
            // holding two multi-GB models at once would exhaust RAM.
            std::lock_guard<std::mutex> lock(cached_mutex_);
            cached_pipeline_.reset();
            cached_model_path_.clear();
            cached_load_signature_.clear();
        }

        auto pipeline = create_pipeline(task.params.task);
        if (!pipeline) {
            result.error_message = "Unknown task type";
            return result;
        }

        // Model loading touches the GPU too (it allocates backend buffers and
        // stages weights), so it can throw the same way generation can.
        bool loaded = false;
        std::string load_exception;
        try {
            loaded = pipeline->load_model(task.params.model_path, task.params);
        } catch (const std::exception& e) {
            load_exception = e.what();
        } catch (...) {
            load_exception = "Unknown backend failure while loading the model";
        }

        if (!load_exception.empty()) {
            result.error_message = "Failed to load model: " + task.params.model_path +
                                   " (" + load_exception + ")";
            return result;
        }

        if (!loaded) {
            std::string err_detail = "Failed to load model: " + task.params.model_path;
            std::error_code ec;
            if (!std::filesystem::exists(task.params.model_path, ec)) {
                err_detail += " (file does not exist on disk)";
            } else {
                auto fsize = std::filesystem::file_size(task.params.model_path, ec);
                if (!ec) {
                    err_detail += " (file size: " + std::to_string(fsize) + " bytes)";
                }
            }
            result.error_message = err_detail;
            return result;
        }

        std::lock_guard<std::mutex> lock(cached_mutex_);
        cached_pipeline_ = std::move(pipeline);
        cached_task_type_ = task.params.task;
        cached_model_path_ = task.params.model_path;
        cached_load_signature_ = want_signature;
    } else {
        std::cout << "[Queue] Reusing loaded model: " << cached_model_path_ << std::endl;
    }

    Pipeline* pipeline_ptr = nullptr;
    {
        std::lock_guard<std::mutex> lock(cached_mutex_);
        pipeline_ptr = cached_pipeline_.get();
        active_pipeline_ = pipeline_ptr;
    }

    // A backend failure often arrives as a C++ exception thrown from deep
    // inside ggml (vulkan.hpp raises one for every non-success VkResult)
    // rather than as a false GenerateResult. Fold that back into a result
    // here so the device-lost recovery below runs for both shapes of
    // failure — letting it propagate instead would skip the cache reset and
    // leave a dead GPU context loaded for every later request.
    try {
        result = pipeline_ptr->run(task.params);
    } catch (const std::exception& e) {
        result.success = false;
        result.error_message = e.what();
    } catch (...) {
        result.success = false;
        result.error_message = "Unknown backend failure during generation";
    }

    {
        std::lock_guard<std::mutex> lock(cached_mutex_);
        active_pipeline_ = nullptr;
    }

    return result;
}

// Throws away the cached pipeline. Called after a device loss, where the GPU
// context it holds is dead and every later request against it would fail too.
void TaskQueue::drop_cached_pipeline() {
    std::lock_guard<std::mutex> lock(cached_mutex_);
    cached_pipeline_.reset();
    cached_model_path_.clear();
    cached_load_signature_.clear();
}

// Hands a result back to whoever queued the task, and never lets an exception
// out. on_complete belongs to the server layer, and this runs on the worker
// thread: an exception that escapes worker_loop escapes the thread function
// itself, which is an immediate std::terminate with no unwinding and no
// message. That is the shape of the silent death this queue exists to survive
// - the process would go down still holding the dead GPU context, and the
// device-loss recovery below would never get to run.
static void notify(const Task& task, const GenerateResult& result) {
    if (!task.on_complete) return;
    try {
        task.on_complete(result);
    } catch (const std::exception& e) {
        std::cerr << "[Queue] Completion callback threw, ignoring: " << e.what() << std::endl;
    } catch (...) {
        std::cerr << "[Queue] Completion callback threw a non-standard exception, ignoring."
                  << std::endl;
    }
}

void TaskQueue::worker_loop() {
    while (running_) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return !queue_.empty() || !running_; });

            if (!running_) break;

            task = std::move(queue_.front());
            queue_.pop();
        }

        cancel_requested_ = false;

        try {
            auto start = std::chrono::steady_clock::now();

            GenerateResult result = run_once(task);

            // A lost GPU device is usually transient: the driver reset the
            // adapter (Windows' TDR watchdog fires after ~2s of a submit not
            // completing) and a context built afterwards works again. What is
            // never recoverable is the context that was live when it happened,
            // so drop it and give the task one more go on a fresh one rather
            // than handing the user an error they can only fix by restarting.
            // An out-of-memory failure is as poisonous as a lost device. The
            // run that failed leaves the cached context in a state that kills
            // the process on the NEXT request: measured on a 6GB card, a
            // 961-frame video OOM'd cleanly, and the following 33-frame request
            // - a size that takes 140s and succeeds on a fresh server - aborted
            // the backend with no error at all. Rebuild rather than reuse.
            if (!result.success && !cancel_requested_.load() &&
                (is_device_lost_error(result.error_message) ||
                 is_out_of_memory_error(result.error_message))) {
                const bool was_oom = is_out_of_memory_error(result.error_message);
                std::cerr << "[Queue] " << (was_oom ? "GPU out of memory" : "GPU device lost")
                          << " (" << result.error_message
                          << "); discarding the cached model so the next request starts from a "
                             "clean context." << std::endl;
                drop_cached_pipeline();

                std::string first_error = result.error_message;

                // Both are now worth retrying, for the same reason: the backoff
                // level feeds the VAE tiling plan, so attempt N+1 really does
                // build smaller graphs than attempt N.
                //
                // This used to bail out immediately on an OOM, on the reasoning
                // that the request was simply too big and a retry would fail
                // identically. That was true when nothing downstream read the
                // backoff level. It is not true now: a video OOM is almost
                // always one oversized VAE decode buffer, and halving the tile
                // budget is exactly the thing that fixes it. Upstream cannot do
                // this for us - prepare_vae_decode_retry_tiling() refuses to act
                // once tiling is already on, so it can only turn tiling on, not
                // make it finer.
                //
                // Escalate more than once for an OOM, because the estimate can
                // be off by several times, but keep it bounded: each attempt
                // costs a full re-run of diffusion.
                const int max_backoff = was_oom ? 3 : 1;

                // Retry with a SMALLER VRAM budget, not the same one. A TDR
                // means one submit ran too long; an identical plan produces
                // identical submits, which is exactly why this used to report
                // "device lost twice in a row" instead of recovering.
                Task retry = task;
                for (int attempt = 0; attempt < max_backoff; ++attempt) {
                    if (cancel_requested_.load()) break;
                    retry.params.vram_backoff = task.params.vram_backoff + attempt + 1;
                    std::cerr << "[Queue] Retrying at VRAM backoff level "
                              << retry.params.vram_backoff
                              << " (smaller graph segments, shorter submits)" << std::endl;
                    result = run_once(retry);
                    if (result.success) break;
                    if (!is_device_lost_error(result.error_message) &&
                        !is_out_of_memory_error(result.error_message)) {
                        break;   // a different failure: retrying will not help
                    }
                    drop_cached_pipeline();
                }

                if (!result.success && is_device_lost_error(result.error_message)) {
                    drop_cached_pipeline();
                    // The raw "vk::Device::waitForFences: ErrorDeviceLost" tells
                    // a user nothing about what to do next, so replace it with
                    // the cause and the knobs that actually change the outcome.
                    result.error_message =
                        "GPU device lost twice in a row (" + first_error + "). The graphics "
                        "driver reset the GPU mid-generation, usually because one submit ran "
                        "past Windows' 2s GPU watchdog (TDR) or the card ran out of VRAM. Try a "
                        "smaller width/height or fewer steps. The model has been unloaded, so "
                        "the next request starts from a fresh GPU context.";
                } else if (!result.success && is_out_of_memory_error(result.error_message)) {
                    result.error_message =
                        "The GPU ran out of memory even after " + std::to_string(max_backoff) +
                        " attempts at progressively smaller VRAM budgets (" + first_error +
                        "). This request is too large for this card - reduce the width/height "
                        "or the frame count.";
                }
            }

            if (cancel_requested_.load()) {
                result.success = false;
                result.error_message = "CancelledByUser";
            }

            auto end = std::chrono::steady_clock::now();
            result.elapsed_seconds = std::chrono::duration<double>(end - start).count();

            notify(task, result);
        } catch (const std::exception& e) {
            notify(task, {false, "", e.what(), 0});
        } catch (...) {
            // run_once already folds everything it can see into a failed
            // result, so nothing should reach here. "Should not" is not
            // "cannot", though, and the cost of being wrong is not this one
            // task - it is the whole server, killed with no log line, which is
            // precisely the failure that took a long time to diagnose. A
            // non-std exception from ggml, a throwing allocator, or a callback
            // that raised something exotic all land here instead.
            notify(task, {false, "", "Unknown fatal error while running the task", 0});
        }
    }
}

} // namespace vison
