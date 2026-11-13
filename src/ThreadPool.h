#ifndef FCT_BASE_THREAD_POOL_H
#define FCT_BASE_THREAD_POOL_H

#include "./ThirdParty.h"
#include "./Status.h"
#include "./Computation.h"

#if defined(_MSC_VER) || defined(__i386__) || defined(__x86_64__) || defined(_M_X64) || defined(_M_IX86)
#include <immintrin.h>
#define FCT_CPU_PAUSE() _mm_pause()
#elif defined(__aarch64__) || defined(__arm__)
#define FCT_CPU_PAUSE() __asm__ __volatile__("yield" ::: "memory")
#else
#define FCT_CPU_PAUSE() std::this_thread::yield()
#endif

namespace FCT {

    enum class JobPriority : uint8_t {
        High = 0,
        Normal = 1,
        Low = 2,
        Count = 3
    };

    /**
     * @brief 栈上零分配的原子任务计数器，配合 ThreadPool::wait 实现协作式等待，避免嵌套并行死锁
     */
    class JobCounter {
    public:
        explicit JobCounter(int32_t initialCount = 0) noexcept
            : m_pending(initialCount) {}

        JobCounter(const JobCounter&) = delete;
        JobCounter& operator=(const JobCounter&) = delete;

        void add(int32_t count = 1) noexcept {
            m_pending.fetch_add(count, std::memory_order_acq_rel);
        }

        void decrement() noexcept {
            m_pending.fetch_sub(1, std::memory_order_acq_rel);
        }

        int32_t pending() const noexcept {
            return m_pending.load(std::memory_order_acquire);
        }

        bool isComplete() const noexcept {
            return pending() <= 0;
        }

    private:
        std::atomic<int32_t> m_pending{0};
    };

    /**
     * @brief 支持工作窃取（Work-Stealing）、三级优先级队列以及调用线程协作消费的通用线程池
     */
    class ThreadPool {
    public:
        explicit ThreadPool(size_t workerCount = 0, std::string poolName = "MQWorker");
        ~ThreadPool();

        ThreadPool(const ThreadPool&) = delete;
        ThreadPool& operator=(const ThreadPool&) = delete;

        static ThreadPool& localInstance();
        static ThreadPool& globalInstance();
        static void setSharedInstance(ThreadPool* shared) noexcept;
        static size_t defaultWorkerCount() noexcept;

        Status start(size_t workerCount = 0);
        void stop() noexcept;
        Status shutdown(bool drainPendingTasks = true);

        bool isRunning() const noexcept { return m_running.load(std::memory_order_acquire); }
        bool isStopping() const noexcept { return m_stopping.load(std::memory_order_acquire); }
        size_t workerCount() const noexcept { return m_workers.size(); }
        size_t pendingTaskCount() const noexcept { return m_pendingTasks.load(std::memory_order_relaxed); }
        size_t activeTaskCount() const noexcept { return m_activeTasks.load(std::memory_order_relaxed); }
        uint64_t completedTaskCount() const noexcept { return m_completedTasks.load(std::memory_order_relaxed); }
        bool isWorkerThread() const noexcept;
        int currentWorkerIndex() const noexcept;

        Status dispatch(
            std::function<void()> task,
            JobPriority priority = JobPriority::Normal,
            JobCounter* counter = nullptr);

        Status dispatchBatch(
            std::vector<std::function<void()>>&& tasks,
            JobPriority priority = JobPriority::Normal,
            JobCounter* counter = nullptr);

        template<typename Callable, typename... Args>
        auto submitWithPriority(JobPriority priority, Callable&& fn, Args&&... args)
            -> std::future<std::invoke_result_t<Callable, Args...>> {
            using ReturnType = std::invoke_result_t<Callable, Args...>;
            auto boundTask = std::make_shared<std::packaged_task<ReturnType()>>(
                [func = std::forward<Callable>(fn), ... capturedArgs = std::forward<Args>(args)]() mutable -> ReturnType {
                    return std::invoke(std::move(func), std::move(capturedArgs)...);
                });
            std::future<ReturnType> future = boundTask->get_future();
            static_cast<void>(dispatch([boundTask]() { (*boundTask)(); }, priority, nullptr));
            return future;
        }

        template<typename Callable, typename... Args>
        auto submit(Callable&& fn, Args&&... args)
            -> std::future<std::invoke_result_t<Callable, Args...>> {
            return submitWithPriority(
                JobPriority::Normal,
                std::forward<Callable>(fn),
                std::forward<Args>(args)...);
        }

        template<typename Callable, typename... Args>
        auto submitAsync(JobPriority priority, Callable&& fn, Args&&... args)
            -> Computation<std::invoke_result_t<std::decay_t<Callable>, std::decay_t<Args>...>> {
            using ReturnType = std::invoke_result_t<std::decay_t<Callable>, std::decay_t<Args>...>;
            using StateType = ComputationSharedState<ReturnType>;

            StateType* state = FCT_NEW(StateType);
            state->addRef();

            auto task = [state, func = std::forward<Callable>(fn), ... capturedArgs = std::forward<Args>(args)]() mutable {
                if constexpr (std::is_void_v<ReturnType>) {
                    std::invoke(std::move(func), std::move(capturedArgs)...);
                    state->setReady();
                } else {
                    state->setValue(std::invoke(std::move(func), std::move(capturedArgs)...));
                }
                state->release();
            };

            Status status = dispatch(std::move(task), priority, nullptr);
            if (!status.ok()) {
                if constexpr (std::is_void_v<ReturnType>) {
                    state->setReady();
                }
                state->release();
            }

            return Computation<ReturnType>::fromSharedState(state);
        }

        template<typename Callable, typename... Args>
        auto submitAsync(Callable&& fn, Args&&... args)
            -> Computation<std::invoke_result_t<std::decay_t<Callable>, std::decay_t<Args>...>> {
            return submitAsync(JobPriority::Normal, std::forward<Callable>(fn), std::forward<Args>(args)...);
        }

        bool tryExecuteOneTask();
        void wait(JobCounter& counter);
        void waitIdle();

        template<typename IndexType, typename BodyFn>
        void parallelFor(
            IndexType begin,
            IndexType end,
            IndexType grainSize,
            BodyFn&& body,
            JobPriority priority = JobPriority::High) {
            if (end <= begin) {
                return;
            }
            const IndexType stepGrain = grainSize > static_cast<IndexType>(0)
                ? grainSize
                : static_cast<IndexType>(1);
            const IndexType totalCount = end - begin;

            if (totalCount <= stepGrain || !isRunning() || workerCount() == 0) {
                for (IndexType i = begin; i < end; ++i) {
                    body(i);
                }
                return;
            }

            const size_t chunkCount = static_cast<size_t>((totalCount + stepGrain - 1) / stepGrain);
            if (chunkCount <= 1) {
                for (IndexType i = begin; i < end; ++i) {
                    body(i);
                }
                return;
            }

            JobCounter counter(static_cast<int32_t>(chunkCount - 1));
            const auto* bodyPtr = &body;

            std::vector<std::function<void()>> batch;
            batch.reserve(chunkCount - 1);
            for (size_t chunkIdx = 1; chunkIdx < chunkCount; ++chunkIdx) {
                const IndexType chunkBegin = begin + static_cast<IndexType>(chunkIdx) * stepGrain;
                const IndexType chunkEnd = std::min<IndexType>(end, chunkBegin + stepGrain);
                batch.emplace_back([bodyPtr, chunkBegin, chunkEnd]() {
                    for (IndexType idx = chunkBegin; idx < chunkEnd; ++idx) {
                        (*bodyPtr)(idx);
                    }
                });
            }
            static_cast<void>(dispatchBatch(std::move(batch), priority, &counter));

            const IndexType firstChunkEnd = std::min<IndexType>(end, begin + stepGrain);
            for (IndexType idx = begin; idx < firstChunkEnd; ++idx) {
                body(idx);
            }

            wait(counter);
        }

    private:
        struct ScheduledJob {
            std::function<void()> fn;
            JobCounter* counter = nullptr;
        };

        struct WorkerQueue {
            mutable std::mutex mutex;
            std::deque<ScheduledJob> deque;
        };

        void workerLoop(size_t workerIndex);
        bool popFromLocalWorker(size_t workerIndex, ScheduledJob& outJob);
        bool popFromGlobalQueues(ScheduledJob& outJob);
        bool stealFromPeerWorkers(size_t thiefIndex, ScheduledJob& outJob);
        bool acquireNextJob(size_t workerIndex, ScheduledJob& outJob);
        void executeScheduledJob(ScheduledJob& job) noexcept;

        std::string m_poolName;
        std::atomic<bool> m_running{false};
        std::atomic<bool> m_stopping{false};
        std::atomic<size_t> m_pendingTasks{0};
        std::atomic<size_t> m_activeTasks{0};
        std::atomic<uint64_t> m_completedTasks{0};
        std::atomic<size_t> m_roundRobinCursor{0};

        std::vector<WorkerQueue*> m_workerQueues;
        std::vector<std::thread> m_workers;

        mutable std::mutex m_shutdownMutex;
        mutable std::mutex m_globalMutex;
        std::condition_variable m_wakeCondition;
        std::condition_variable m_idleCondition;
        std::array<std::deque<ScheduledJob>, static_cast<size_t>(JobPriority::Count)> m_priorityQueues;
    };

} // namespace FCT

#endif // FCT_BASE_THREAD_POOL_H
