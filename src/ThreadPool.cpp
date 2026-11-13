#include "./ThreadPool.h"
#include "./MemoryCheak.h"
#include <cassert>

namespace FCT {

    namespace {
        std::atomic<ThreadPool*> g_sharedThreadPool{nullptr};
        thread_local ThreadPool* tls_currentPool = nullptr;
        thread_local int tls_workerIndex = -1;
    }

    size_t ThreadPool::defaultWorkerCount() noexcept {
        const unsigned int hw = std::thread::hardware_concurrency();
        if (hw <= 2u) {
            return 2u;
        }
        return static_cast<size_t>(hw - 1u);
    }

    ThreadPool::ThreadPool(size_t workerCount, std::string poolName)
        : m_poolName(poolName.empty() ? "MQWorker" : std::move(poolName)) {
        start(workerCount);
    }

    ThreadPool::~ThreadPool() {
        shutdown(true);
    }

    ThreadPool& ThreadPool::localInstance() {
        static ThreadPool s_localPool(defaultWorkerCount(), "FCTWorker");
        return s_localPool;
    }

    ThreadPool& ThreadPool::globalInstance() {
        ThreadPool* shared = g_sharedThreadPool.load(std::memory_order_acquire);
        return shared ? *shared : localInstance();
    }

    void ThreadPool::setSharedInstance(ThreadPool* shared) noexcept {
        g_sharedThreadPool.store(shared, std::memory_order_release);
    }

    Status ThreadPool::start(size_t workerCount) {
        bool expected = false;
        if (!m_running.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return OkStatus();
        }
        m_stopping.store(false, std::memory_order_release);

        const size_t resolvedCount = workerCount > 0 ? workerCount : defaultWorkerCount();
        m_workerQueues.clear();
        m_workerQueues.reserve(resolvedCount);
        for (size_t i = 0; i < resolvedCount; ++i) {
            m_workerQueues.push_back(FCT_NEW(WorkerQueue));
        }

        m_workers.reserve(resolvedCount);
        for (size_t i = 0; i < resolvedCount; ++i) {
            m_workers.emplace_back([this, i]() {
                workerLoop(i);
            });
        }
        return OkStatus();
    }

    void ThreadPool::stop() noexcept {
        if (!isRunning() || m_stopping.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        m_wakeCondition.notify_all();
    }

    Status ThreadPool::shutdown(bool drainPendingTasks) {
        assert(!isWorkerThread() && "ThreadPool::shutdown 禁止在池内工作线程中调用，以防止自加入死锁");
        if (isWorkerThread()) {
            return FailedPreconditionError("Cannot call shutdown from within a worker thread");
        }

        std::lock_guard<std::mutex> lock(m_shutdownMutex);
        if (!isRunning()) {
            return OkStatus();
        }

        // 阶段一：停止接收新任务
        stop();

        // 阶段二：排空未完成任务
        if (drainPendingTasks) {
            while (m_pendingTasks.load(std::memory_order_acquire) > 0) {
                if (!tryExecuteOneTask()) {
                    break;
                }
            }
            std::unique_lock<std::mutex> lock(m_globalMutex);
            m_idleCondition.wait(lock, [this]() {
                return m_pendingTasks.load(std::memory_order_acquire) == 0 &&
                       m_activeTasks.load(std::memory_order_acquire) == 0;
            });
        } else {
            {
                std::lock_guard<std::mutex> lock(m_globalMutex);
                for (auto& q : m_priorityQueues) {
                    for (auto& job : q) {
                        if (job.counter) job.counter->decrement();
                    }
                    q.clear();
                }
            }
            for (auto* wq : m_workerQueues) {
                std::lock_guard<std::mutex> lock(wq->mutex);
                for (auto& job : wq->deque) {
                    if (job.counter) job.counter->decrement();
                }
                wq->deque.clear();
            }
            m_pendingTasks.store(0, std::memory_order_release);
        }

        // 唤醒所有 Worker 退出循环并汇聚
        m_wakeCondition.notify_all();

        for (auto& worker : m_workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        m_workers.clear();

        for (WorkerQueue* q : m_workerQueues) {
            FCT_DELETE(q);
        }
        m_workerQueues.clear();

        m_running.store(false, std::memory_order_release);
        return OkStatus();
    }

    bool ThreadPool::isWorkerThread() const noexcept {
        return tls_currentPool == this && tls_workerIndex >= 0;
    }

    int ThreadPool::currentWorkerIndex() const noexcept {
        return (tls_currentPool == this) ? tls_workerIndex : -1;
    }

    Status ThreadPool::dispatch(std::function<void()> task, JobPriority priority, JobCounter* counter) {
        assert(task && "dispatch 任务函数指针不能为空");
        if (!task) {
            return InvalidArgumentError("Task function cannot be null");
        }

        if (!isRunning()) {
            if (counter) {
                counter->decrement();
            }
            return UnavailableError("ThreadPool is not running");
        }

        if (m_stopping.load(std::memory_order_acquire)) {
            if (counter) {
                counter->decrement();
            }
            return UnavailableError("ThreadPool is stopping; new tasks rejected");
        }

        m_pendingTasks.fetch_add(1, std::memory_order_acq_rel);
        ScheduledJob scheduled{std::move(task), counter};

        const int localWorker = currentWorkerIndex();
        if (localWorker >= 0 && priority == JobPriority::High &&
            static_cast<size_t>(localWorker) < m_workerQueues.size()) {
            {
                std::lock_guard<std::mutex> lock(m_workerQueues[static_cast<size_t>(localWorker)]->mutex);
                m_workerQueues[static_cast<size_t>(localWorker)]->deque.push_back(std::move(scheduled));
            }
            m_wakeCondition.notify_one();
            return OkStatus();
        }

        const size_t prioIdx = static_cast<size_t>(priority) < m_priorityQueues.size()
            ? static_cast<size_t>(priority)
            : static_cast<size_t>(JobPriority::Normal);
        {
            std::lock_guard<std::mutex> lock(m_globalMutex);
            m_priorityQueues[prioIdx].push_back(std::move(scheduled));
        }
        m_wakeCondition.notify_one();
        return OkStatus();
    }

    Status ThreadPool::dispatchBatch(
        std::vector<std::function<void()>>&& tasks,
        JobPriority priority,
        JobCounter* counter) {
        if (tasks.empty()) {
            return OkStatus();
        }

        if (!isRunning()) {
            if (counter) {
                for (const auto& fn : tasks) {
                    if (fn) counter->decrement();
                }
            }
            return UnavailableError("ThreadPool is not running");
        }

        if (m_stopping.load(std::memory_order_acquire)) {
            if (counter) {
                for (const auto& fn : tasks) {
                    if (fn) counter->decrement();
                }
            }
            return UnavailableError("ThreadPool is stopping; new tasks rejected");
        }

        const size_t workerNums = m_workerQueues.size();
        size_t validCount = 0;
        for (const auto& fn : tasks) {
            if (fn) {
                ++validCount;
            }
        }
        if (validCount == 0) {
            return OkStatus();
        }

        m_pendingTasks.fetch_add(validCount, std::memory_order_acq_rel);

        if (priority == JobPriority::High && workerNums > 0) {
            const size_t startCursor = m_roundRobinCursor.fetch_add(validCount, std::memory_order_relaxed);
            size_t idx = 0;
            for (auto& fn : tasks) {
                if (!fn) {
                    continue;
                }
                const size_t targetWorker = (startCursor + idx) % workerNums;
                std::lock_guard<std::mutex> lock(m_workerQueues[targetWorker]->mutex);
                m_workerQueues[targetWorker]->deque.push_back(ScheduledJob{std::move(fn), counter});
                ++idx;
            }
            m_wakeCondition.notify_all();
            return OkStatus();
        }

        const size_t prioIdx = static_cast<size_t>(priority) < m_priorityQueues.size()
            ? static_cast<size_t>(priority)
            : static_cast<size_t>(JobPriority::Normal);
        {
            std::lock_guard<std::mutex> lock(m_globalMutex);
            for (auto& fn : tasks) {
                if (fn) {
                    m_priorityQueues[prioIdx].push_back(ScheduledJob{std::move(fn), counter});
                }
            }
        }
        m_wakeCondition.notify_all();
        return OkStatus();
    }

    bool ThreadPool::popFromLocalWorker(size_t workerIndex, ScheduledJob& outJob) {
        if (workerIndex >= m_workerQueues.size()) {
            return false;
        }
        auto& q = *m_workerQueues[workerIndex];
        std::lock_guard<std::mutex> lock(q.mutex);
        if (q.deque.empty()) {
            return false;
        }
        outJob = std::move(q.deque.back());
        q.deque.pop_back();
        return true;
    }

    bool ThreadPool::popFromGlobalQueues(ScheduledJob& outJob) {
        std::unique_lock<std::mutex> lock(m_globalMutex, std::try_to_lock);
        if (!lock.owns_lock()) {
            return false;
        }
        for (auto& queue : m_priorityQueues) {
            if (!queue.empty()) {
                outJob = std::move(queue.front());
                queue.pop_front();
                return true;
            }
        }
        return false;
    }

    bool ThreadPool::stealFromPeerWorkers(size_t thiefIndex, ScheduledJob& outJob) {
        const size_t count = m_workerQueues.size();
        if (count == 0) {
            return false;
        }
        for (size_t offset = 1; offset <= count; ++offset) {
            const size_t victimIdx = (thiefIndex + offset) % count;
            if (victimIdx == thiefIndex && count > 1) {
                continue;
            }
            auto& victim = *m_workerQueues[victimIdx];
            std::unique_lock<std::mutex> lock(victim.mutex, std::try_to_lock);
            if (!lock.owns_lock() || victim.deque.empty()) {
                continue;
            }
            outJob = std::move(victim.deque.front());
            victim.deque.pop_front();
            return true;
        }
        return false;
    }

    bool ThreadPool::acquireNextJob(size_t workerIndex, ScheduledJob& outJob) {
        if (m_pendingTasks.load(std::memory_order_acquire) == 0) {
            return false;
        }
        if (isWorkerThread() && currentWorkerIndex() == static_cast<int>(workerIndex)) {
            if (popFromLocalWorker(workerIndex, outJob)) {
                return true;
            }
        }
        if (popFromGlobalQueues(outJob)) {
            return true;
        }
        return stealFromPeerWorkers(workerIndex, outJob);
    }

    void ThreadPool::executeScheduledJob(ScheduledJob& job) noexcept {
        m_activeTasks.fetch_add(1, std::memory_order_acq_rel);
        m_pendingTasks.fetch_sub(1, std::memory_order_acq_rel);

        assert(job.fn && "ScheduledJob::fn 任务可调用对象不能为空");
        if (job.fn) {
            job.fn();
        }

        if (job.counter) {
            job.counter->decrement();
        }
        m_completedTasks.fetch_add(1, std::memory_order_relaxed);
        const size_t remainingActive = m_activeTasks.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remainingActive == 0 && m_pendingTasks.load(std::memory_order_acquire) == 0) {
            std::lock_guard<std::mutex> lock(m_globalMutex);
            m_idleCondition.notify_all();
        }
    }

    bool ThreadPool::tryExecuteOneTask() {
        const int localIdx = currentWorkerIndex();
        const size_t probeIdx = localIdx >= 0
            ? static_cast<size_t>(localIdx)
            : (m_roundRobinCursor.fetch_add(1, std::memory_order_relaxed) % std::max<size_t>(1, m_workerQueues.size()));

        ScheduledJob job;
        if (!acquireNextJob(probeIdx, job)) {
            return false;
        }
        executeScheduledJob(job);
        return true;
    }

    void ThreadPool::wait(JobCounter& counter) {
        ZoneScoped();
        uint32_t spinCount = 0;
        while (!counter.isComplete()) {
            if (tryExecuteOneTask()) {
                spinCount = 0;
            } else {
                if (spinCount < 32) {
                    FCT_CPU_PAUSE();
                } else if (spinCount < 64) {
                    std::this_thread::yield();
                } else {
                    std::this_thread::sleep_for(std::chrono::microseconds(50));
                }
                ++spinCount;
            }
        }
    }

    void ThreadPool::waitIdle() {
        ZoneScoped();
        if (isWorkerThread()) {
            while (m_pendingTasks.load(std::memory_order_acquire) > 0) {
                if (!tryExecuteOneTask()) {
                    break;
                }
            }
            return;
        }

        std::unique_lock<std::mutex> lock(m_globalMutex);
        m_idleCondition.wait(lock, [this]() {
            return m_pendingTasks.load(std::memory_order_acquire) == 0 &&
                   m_activeTasks.load(std::memory_order_acquire) == 0;
        });
    }

    void ThreadPool::workerLoop(size_t workerIndex) {
        tls_currentPool = this;
        tls_workerIndex = static_cast<int>(workerIndex);

        const std::string threadName = m_poolName + "_" + std::to_string(workerIndex);
        SetThreadName(threadName.c_str());

        while (true) {
            ScheduledJob job;
            if (acquireNextJob(workerIndex, job)) {
                ZoneScoped();
                executeScheduledJob(job);
                continue;
            }

            std::unique_lock<std::mutex> lock(m_globalMutex);
            if (m_stopping.load(std::memory_order_acquire) &&
                m_pendingTasks.load(std::memory_order_acquire) == 0) {
                break;
            }
            if (m_pendingTasks.load(std::memory_order_acquire) == 0) {
                m_wakeCondition.wait_for(lock, std::chrono::milliseconds(4), [this]() {
                    return m_stopping.load(std::memory_order_acquire) ||
                           m_pendingTasks.load(std::memory_order_acquire) > 0;
                });
            }
        }

        tls_currentPool = nullptr;
        tls_workerIndex = -1;
    }

} // namespace FCT
