#include "core/JobSystem.hpp"

#include <algorithm>

namespace elysium {

thread_local bool JobSystem::workerThread_ = false;

JobSystem::JobSystem(std::size_t requestedWorkers) {
    const unsigned hw = std::thread::hardware_concurrency();
    const std::size_t defaultCount = hw > 1 ? static_cast<std::size_t>(hw - 1) : 1U;
    const std::size_t count = std::clamp(requestedWorkers == 0 ? defaultCount : requestedWorkers,
                                         std::size_t{1}, std::size_t{8});
    workers_.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        workers_.emplace_back([this]() { workerLoop(); });
    }
}

JobSystem::JobSystem(SerialJobSystemTag) : serialMode_(true) {}

JobSystem::~JobSystem() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    cv_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) worker.join();
    }
}

std::size_t JobSystem::queuedJobs() const {
    std::lock_guard lock(mutex_);
    return queue_.size();
}

bool JobSystem::isWorkerThread() { return workerThread_; }

void JobSystem::workerLoop() {
    workerThread_=true;
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this]() { return stopping_ || !queue_.empty(); });
            if (stopping_ && queue_.empty()) return;
            job = std::move(queue_.front());
            queue_.pop();
        }
        job();
    }
}

} // namespace elysium
