#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace elysium {

struct SerialJobSystemTag { explicit constexpr SerialJobSystemTag() = default; };
inline constexpr SerialJobSystemTag SerialJobs{};

// One engine-wide bounded worker pool. Jobs compute immutable CPU-side results;
// authoritative world/ECS mutation and graphics publication stay on the owner thread.
class JobSystem {
public:
    explicit JobSystem(std::size_t requestedWorkers = 0);
    explicit JobSystem(SerialJobSystemTag);
    ~JobSystem();

    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    std::size_t workerCount() const { return workers_.size(); }
    bool serialMode() const { return serialMode_; }
    std::size_t queuedJobs() const;

    template <class Fn>
    auto submit(Fn&& fn) -> std::future<std::invoke_result_t<Fn>> {
        using Result = std::invoke_result_t<Fn>;
        auto task = std::make_shared<std::packaged_task<Result()>>(std::forward<Fn>(fn));
        auto future = task->get_future();
        if (serialMode_) {
            (*task)();
            return future;
        }
        {
            std::lock_guard lock(mutex_);
            if (stopping_) throw std::runtime_error("JobSystem is stopping");
            queue_.emplace([task]() { (*task)(); });
        }
        cv_.notify_one();
        return future;
    }

    // Synchronous deterministic partition helper for read-heavy phases such as
    // AI sense/think and batch generation. The caller participates in the
    // work; nested calls from a worker fall back to serial execution so a job
    // can never deadlock waiting for the same bounded pool.
    template <class Fn>
    void parallelFor(std::size_t count, Fn&& fn, std::size_t grain = 1) {
        if (count == 0) return;
        grain = std::max<std::size_t>(1, grain);
        if (workers_.size() <= 1 || isWorkerThread() || count <= grain) {
            for (std::size_t i=0;i<count;++i) fn(i);
            return;
        }

        std::atomic<std::size_t> next{0};
        const std::size_t taskCount = std::min<std::size_t>(workers_.size(), (count + grain - 1) / grain);
        auto worker=[&]() {
            for (;;) {
                const std::size_t begin=next.fetch_add(grain,std::memory_order_relaxed);
                if (begin>=count) return;
                const std::size_t end=std::min(count,begin+grain);
                for(std::size_t i=begin;i<end;++i) fn(i);
            }
        };

        std::vector<std::future<void>> futures;
        futures.reserve(taskCount>0?taskCount-1:0);
        for(std::size_t t=1;t<taskCount;++t) futures.push_back(submit(worker));
        worker();
        for(auto& f:futures) f.get();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<std::function<void()>> queue_;
    std::vector<std::thread> workers_;
    bool stopping_{};
    bool serialMode_{};

    void workerLoop();
    static bool isWorkerThread();
    static thread_local bool workerThread_;
};

} // namespace elysium
