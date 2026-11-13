#ifndef FCT_BASE_COMPUTATION_H
#define FCT_BASE_COMPUTATION_H

#ifndef FCT_COMPUTATION_H
#define FCT_COMPUTATION_H
#endif

#include "./ThirdParty.h"
#include "./MemoryCheak.h"
#include "./RefCount.h"
#include "./Status.h"
#include <variant>
#include <optional>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <utility>
#include <cassert>
#include <type_traits>

namespace FCT {

    /**
     * @brief 异步计算泛型共享状态，托管计算结果与跨线程唤醒同步
     * 继承自 FCT::NewRefCount，使用侵入式原子引用计数与 FCT_DELETE 生命周期管理，严格杜绝 std::shared_ptr
     */
    template<typename T>
    class ComputationSharedState : public NewRefCount<ComputationSharedState<T>, true, false> {
    public:
        ComputationSharedState() = default;
        virtual ~ComputationSharedState() = default;

        void setValue(T&& val) {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_value.emplace(std::move(val));
                m_ready.store(true, std::memory_order_release);
            }
            m_cv.notify_all();
        }

        void setValue(const T& val) requires std::copy_constructible<T> {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_value.emplace(val);
                m_ready.store(true, std::memory_order_release);
            }
            m_cv.notify_all();
        }

        bool isReady() const noexcept {
            return m_ready.load(std::memory_order_acquire);
        }

        void wait() {
            if (isReady()) {
                return;
            }
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this]() {
                return m_ready.load(std::memory_order_acquire);
            });
        }

        T takeValue() {
            std::lock_guard<std::mutex> lock(m_mutex);
            assert(m_value.has_value() && "无法从空的共享状态中 takeValue");
            T val = std::move(*m_value);
            m_value.reset();
            return val;
        }

    protected:
        void deleteThis() override {
            FCT_DELETE(this);
        }

    private:
        std::atomic<bool> m_ready{false};
        std::optional<T> m_value;
        mutable std::mutex m_mutex;
        std::condition_variable m_cv;
    };

    /**
     * @brief void 特化的异步计算共享状态
     */
    template<>
    class ComputationSharedState<void> : public NewRefCount<ComputationSharedState<void>, true, false> {
    public:
        ComputationSharedState() = default;
        virtual ~ComputationSharedState() = default;

        void setReady() {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_ready.store(true, std::memory_order_release);
            }
            m_cv.notify_all();
        }

        bool isReady() const noexcept {
            return m_ready.load(std::memory_order_acquire);
        }

        void wait() {
            if (isReady()) {
                return;
            }
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this]() {
                return m_ready.load(std::memory_order_acquire);
            });
        }

    protected:
        void deleteThis() override {
            FCT_DELETE(this);
        }

    private:
        std::atomic<bool> m_ready{false};
        mutable std::mutex m_mutex;
        std::condition_variable m_cv;
    };

    /**
     * @brief 专属侵入式智能指针辅助类，杜绝使用 std::shared_ptr 与 std::unique_ptr
     */
    template<typename StateType>
    class ComputationIntrusivePtr {
    public:
        ComputationIntrusivePtr() noexcept : m_ptr(nullptr) {}
        explicit ComputationIntrusivePtr(StateType* ptr) noexcept : m_ptr(ptr) {}

        ~ComputationIntrusivePtr() {
            if (m_ptr) {
                m_ptr->release();
                m_ptr = nullptr;
            }
        }

        ComputationIntrusivePtr(const ComputationIntrusivePtr& other) noexcept : m_ptr(other.m_ptr) {
            if (m_ptr) {
                m_ptr->addRef();
            }
        }

        ComputationIntrusivePtr(ComputationIntrusivePtr&& other) noexcept : m_ptr(other.m_ptr) {
            other.m_ptr = nullptr;
        }

        ComputationIntrusivePtr& operator=(const ComputationIntrusivePtr& other) noexcept {
            if (this != &other) {
                if (other.m_ptr) {
                    other.m_ptr->addRef();
                }
                if (m_ptr) {
                    m_ptr->release();
                }
                m_ptr = other.m_ptr;
            }
            return *this;
        }

        ComputationIntrusivePtr& operator=(ComputationIntrusivePtr&& other) noexcept {
            if (this != &other) {
                if (m_ptr) {
                    m_ptr->release();
                }
                m_ptr = other.m_ptr;
                other.m_ptr = nullptr;
            }
            return *this;
        }

        StateType* get() const noexcept { return m_ptr; }
        StateType* operator->() const noexcept { return m_ptr; }
        explicit operator bool() const noexcept { return m_ptr != nullptr; }

    private:
        StateType* m_ptr = nullptr;
    };

    /**
     * @brief 现代 C++20 Computation (FutureOrValue) 统一抽象
     * 
     * 统一表示即时值（Value）与异步计算（Future）：
     * 1. 即时值存储于 std::variant 内部，保证 0 堆内存分配（Zero Heap Allocation）。
     * 2. 异步计算托管于侵入式引用计数共享状态，基于 FCT_NEW / FCT_DELETE 管理。
     * 3. 支持非阻塞 isReady() 查询与懒解析缓存（Lazy Resolution Caching）。
     * 4. 移动语义（Move-Only），完全兼容 UniquePtr<T> 与标准容器。
     */
    template<typename T>
    class Computation {
    public:
        using ValueType = T;
        using SharedStateType = ComputationSharedState<T>;

        Computation() noexcept : m_storage(std::in_place_type<std::monostate>) {}

        explicit Computation(T val) noexcept(std::is_nothrow_move_constructible_v<T>)
            : m_storage(std::in_place_type<T>, std::move(val)) {}

        template<typename... Args>
        explicit Computation(std::in_place_t, Args&&... args)
            : m_storage(std::in_place_type<T>, std::forward<Args>(args)...) {}

        static Computation fromValue(T val) {
            return Computation(std::move(val));
        }

        static Computation fromSharedState(SharedStateType* state) {
            Computation comp;
            comp.m_storage.template emplace<ComputationIntrusivePtr<SharedStateType>>(state);
            return comp;
        }

        // 禁止拷贝，仅允许移动 (Move-Only)
        Computation(const Computation&) = delete;
        Computation& operator=(const Computation&) = delete;

        Computation(Computation&& other) noexcept
            : m_storage(std::move(other.m_storage)) {
            other.m_storage.template emplace<std::monostate>();
        }

        Computation& operator=(Computation&& other) noexcept {
            if (this != &other) {
                m_storage = std::move(other.m_storage);
                other.m_storage.template emplace<std::monostate>();
            }
            return *this;
        }

        ~Computation() = default;

        bool isEmpty() const noexcept {
            return std::holds_alternative<std::monostate>(m_storage);
        }

        bool empty() const noexcept {
            return isEmpty();
        }

        bool isValue() const noexcept {
            return std::holds_alternative<T>(m_storage);
        }

        bool isFuture() const noexcept {
            return std::holds_alternative<ComputationIntrusivePtr<SharedStateType>>(m_storage);
        }

        /**
         * @brief 非阻塞就绪状态查询与懒解析缓存 (Lazy Resolution Caching)
         * 
         * 1. 若内部已为 Value，仅耗费 1 CPU 周期判断直接返回 true。
         * 2. 若内部为 Future，非阻塞检测原子就绪标志；若已完成，原地将其值提取并转移至 variant 的 Value 分支，
         *    并释放 SharedState 引用，使后续所有 isReady/tryGet/take 降阶为 1 周期的内联内存操作。
         */
        bool isReady() const {
            if (std::holds_alternative<T>(m_storage)) {
                return true;
            }
            if (std::holds_alternative<ComputationIntrusivePtr<SharedStateType>>(m_storage)) {
                auto& futurePtr = std::get<ComputationIntrusivePtr<SharedStateType>>(m_storage);
                if (futurePtr && futurePtr->isReady()) {
                    // 懒解析缓存：提取值并原地转移至 variant 的 Value 分支
                    T val = futurePtr->takeValue();
                    m_storage.template emplace<T>(std::move(val));
                    return true;
                }
            }
            return false;
        }

        /**
         * @brief 非阻塞借用内部值指针
         * @return 若已就绪返回内部值裸指针；若未就绪或已转移则安全返回 nullptr
         */
        T* tryGet() noexcept {
            if (!isReady()) {
                return nullptr;
            }
            return std::get_if<T>(&m_storage);
        }

        const T* tryGet() const noexcept {
            if (!isReady()) {
                return nullptr;
            }
            return std::get_if<T>(&m_storage);
        }

        /**
         * @brief 移动提取内部值的所有权
         * 前置契约：必须满足 isReady() == true。提取后 Computation 恢复为 empty 状态。
         */
        T take() {
            assert(isReady() && "Computation::take() 要求 isReady() == true");
            T val = std::move(std::get<T>(m_storage));
            m_storage.template emplace<std::monostate>();
            return val;
        }

        /**
         * @brief 阻塞等待计算完成并提取结果
         */
        T get() {
            if (!isReady()) {
                assert(isFuture() && "get() 时的 Computation 状态非法");
                auto& futurePtr = std::get<ComputationIntrusivePtr<SharedStateType>>(m_storage);
                futurePtr->wait();
                T val = futurePtr->takeValue();
                m_storage.template emplace<std::monostate>();
                return val;
            }
            return take();
        }

        /**
         * @brief 阻塞等待计算完成并触发懒解析缓存
         */
        void wait() const {
            if (isReady()) {
                return;
            }
            assert(isFuture() && "wait() 时的 Computation 状态非法");
            auto& futurePtr = std::get<ComputationIntrusivePtr<SharedStateType>>(m_storage);
            futurePtr->wait();
            T val = futurePtr->takeValue();
            m_storage.template emplace<T>(std::move(val));
        }

    private:
        using StorageType = std::variant<std::monostate, T, ComputationIntrusivePtr<SharedStateType>>;
        mutable StorageType m_storage;
    };

    /**
     * @brief Computation<void> 全特化版本，用于表示无载荷的异步就绪/完成信号
     */
    template<>
    class Computation<void> {
    public:
        using ValueType = void;
        using SharedStateType = ComputationSharedState<void>;

        struct VoidReady {};

        Computation() noexcept : m_storage(std::in_place_type<std::monostate>) {}

        static Computation ready() {
            Computation comp;
            comp.m_storage.template emplace<VoidReady>();
            return comp;
        }

        static Computation fromReady() {
            return ready();
        }

        static Computation fromValue() {
            return ready();
        }

        static Computation fromSharedState(SharedStateType* state) {
            Computation comp;
            comp.m_storage.template emplace<ComputationIntrusivePtr<SharedStateType>>(state);
            return comp;
        }

        Computation(const Computation&) = delete;
        Computation& operator=(const Computation&) = delete;

        Computation(Computation&& other) noexcept
            : m_storage(std::move(other.m_storage)) {
            other.m_storage.template emplace<std::monostate>();
        }

        Computation& operator=(Computation&& other) noexcept {
            if (this != &other) {
                m_storage = std::move(other.m_storage);
                other.m_storage.template emplace<std::monostate>();
            }
            return *this;
        }

        ~Computation() = default;

        bool isEmpty() const noexcept {
            return std::holds_alternative<std::monostate>(m_storage);
        }

        bool empty() const noexcept {
            return isEmpty();
        }

        bool isValue() const noexcept {
            return std::holds_alternative<VoidReady>(m_storage);
        }

        bool isFuture() const noexcept {
            return std::holds_alternative<ComputationIntrusivePtr<SharedStateType>>(m_storage);
        }

        bool isReady() const {
            if (std::holds_alternative<VoidReady>(m_storage)) {
                return true;
            }
            if (std::holds_alternative<ComputationIntrusivePtr<SharedStateType>>(m_storage)) {
                auto& futurePtr = std::get<ComputationIntrusivePtr<SharedStateType>>(m_storage);
                if (futurePtr && futurePtr->isReady()) {
                    m_storage.template emplace<VoidReady>();
                    return true;
                }
            }
            return false;
        }

        bool tryGet() const noexcept {
            return isReady();
        }

        void wait() const {
            if (isReady()) {
                return;
            }
            assert(isFuture() && "wait() 时的 Computation<void> 状态非法");
            auto& futurePtr = std::get<ComputationIntrusivePtr<SharedStateType>>(m_storage);
            futurePtr->wait();
            m_storage.template emplace<VoidReady>();
        }

        void take() {
            assert(isReady() && "Computation<void>::take() 要求 isReady() == true");
            m_storage.template emplace<std::monostate>();
        }

        void get() {
            wait();
            take();
        }

    private:
        using StorageType = std::variant<std::monostate, VoidReady, ComputationIntrusivePtr<SharedStateType>>;
        mutable StorageType m_storage;
    };

} // namespace FCT

#endif // FCT_BASE_COMPUTATION_H
