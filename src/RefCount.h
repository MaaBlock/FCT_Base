#ifndef FCT_BASE_REFCOUNT_H
#define FCT_BASE_REFCOUNT_H

#include <atomic>
#include <functional>
#include <type_traits>
#include <utility>
#include "./MemoryCheak.h"

#ifndef FCT_REFCOUNT_USE_CRTP
#define FCT_REFCOUNT_USE_CRTP 1
#endif

namespace FCT {
    template<typename Derived = void, bool UsedByMutilThread = false, bool UseCRTP = (!std::is_void_v<Derived> && (FCT_REFCOUNT_USE_CRTP != 0))>
    class NewRefCount;

    /**
     * @cond CHINESE
     * @brief 用于自定义对象销毁行为（如对象池回收）
     * @tparam T 子类类型
     * @endcond
     */
    template<typename T>
    class DeletableTrait {
    public:
        using DeleterType = std::function<void(T*)>;

        void setDeleter(DeleterType deleter) {
            m_deleter = std::move(deleter);
        }

        template<typename Func>
        void setDeleterFunc(Func&& func) {
            m_deleter = [f = std::forward<Func>(func)](T* ptr) { f(ptr); };
        }

    protected:
        template<typename Derived, bool UsedByMutilThread, bool UseCRTP>
        friend class NewRefCount;

        bool applyDeleter(T* ptr) {
            if (m_deleter) {
                m_deleter(ptr);
                return true;
            }
            return false;
        }

    private:
        DeleterType m_deleter;
    };

    template<typename T, typename U = T>
    struct IsDeletableTrait : std::is_base_of<DeletableTrait<U>, T> {};

    /**
     * @cond CHINESE
     * @brief 基于 CRTP 的侵入式引用计数基类（静态分发，避免虚表与原子操作开销）
     * @tparam Derived 派生类类型
     * @tparam UsedByMutilThread 是否在多线程环境下使用（false 使用 size_t，true 使用 std::atomic<size_t>）
     * @tparam UseCRTP 是否使用 CRTP 静态分发（true 为 CRTP 静态分发，false 为虚函数多态分发）
     * @endcond
     */
    template<typename Derived, bool UsedByMutilThread, bool UseCRTP>
    class NewRefCount {
        static_assert(!std::is_void_v<Derived>, "CRTP mode requires a non-void Derived type.");
    public:
        using RefCountValueType = std::conditional_t<UsedByMutilThread, std::atomic<size_t>, size_t>;

        constexpr NewRefCount() noexcept : m_refCount(1) {}
        constexpr explicit NewRefCount(size_t refCount) noexcept : m_refCount(refCount) {}
        ~NewRefCount() = default;

        void addRef() noexcept {
            if constexpr (UsedByMutilThread) {
                m_refCount.fetch_add(1, std::memory_order_relaxed);
            } else {
                ++m_refCount;
            }
        }

        void release() {
            if constexpr (UsedByMutilThread) {
                if (m_refCount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    static_cast<Derived*>(this)->deleteThis();
                }
            } else {
                if (--m_refCount == 0) {
                    static_cast<Derived*>(this)->deleteThis();
                }
            }
        }

        size_t count() const noexcept {
            if constexpr (UsedByMutilThread) {
                return m_refCount.load(std::memory_order_relaxed);
            } else {
                return m_refCount;
            }
        }

        operator bool() const noexcept {
            return count() > 0;
        }

    protected:
        void deleteThis() {
            Derived* self = static_cast<Derived*>(this);
            if constexpr (IsDeletableTrait<Derived>::value) {
                if (self->applyDeleter(self)) {
                    return;
                }
            }
            FCT_DELETE(self);
        }

        RefCountValueType m_refCount;
    };

    /**
     * @cond CHINESE
     * @brief 基于虚函数继承的侵入式引用计数基类特化（支持虚析构与虚函数重写 deleteThis）
     * @endcond
     */
    template<typename Derived, bool UsedByMutilThread>
    class NewRefCount<Derived, UsedByMutilThread, false> {
    public:
        using RefCountValueType = std::conditional_t<UsedByMutilThread, std::atomic<size_t>, size_t>;

        constexpr NewRefCount() noexcept : m_refCount(1) {}
        constexpr explicit NewRefCount(size_t refCount) noexcept : m_refCount(refCount) {}
        virtual ~NewRefCount() = default;

        void addRef() noexcept {
            if constexpr (UsedByMutilThread) {
                m_refCount.fetch_add(1, std::memory_order_relaxed);
            } else {
                ++m_refCount;
            }
        }

        void release() {
            if constexpr (UsedByMutilThread) {
                if (m_refCount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    deleteThis();
                }
            } else {
                if (--m_refCount == 0) {
                    deleteThis();
                }
            }
        }

        size_t count() const noexcept {
            if constexpr (UsedByMutilThread) {
                return m_refCount.load(std::memory_order_relaxed);
            } else {
                return m_refCount;
            }
        }

        operator bool() const noexcept {
            return count() > 0;
        }

    protected:
        virtual void deleteThis() {
            if constexpr (!std::is_void_v<Derived>) {
                Derived* self = static_cast<Derived*>(this);
                if constexpr (IsDeletableTrait<Derived>::value) {
                    if (self->applyDeleter(self)) {
                        return;
                    }
                }
                FCT_DELETE(self);
            } else {
                FCT_DELETE(this);
            }
        }

        RefCountValueType m_refCount;
    };

    template<typename Derived = void, bool UsedByMutilThread = false, bool UseCRTP = !std::is_void_v<Derived>>
    using RefCounted = NewRefCount<Derived, UsedByMutilThread, UseCRTP>;

    /**
     * @cond CHINESE
     * @brief 用于切换 CRTP 或普通虚函数继承的模板别名
     * @endcond
     */
    template<typename Derived, bool UsedByMutilThread = false, bool UseCRTP = (FCT_REFCOUNT_USE_CRTP != 0)>
    using RefCountExtend = NewRefCount<Derived, UsedByMutilThread, UseCRTP>;

    template<typename T>
    struct IsNewRefCountBase {
    private:
        template<typename Derived, bool UsedByMutilThread, bool UseCRTP>
        static constexpr bool test(const NewRefCount<Derived, UsedByMutilThread, UseCRTP>*) { return true; }
        static constexpr bool test(...) { return false; }
    public:
        static constexpr bool value = test(static_cast<const std::remove_cv_t<T>*>(nullptr));
        static constexpr bool ret = value;
    };

    template<typename T>
        requires IsNewRefCountBase<T>::value
    inline void safeAddRef(T* obj) {
        if (obj) {
            obj->addRef();
        }
    }

    template<typename T>
        requires IsNewRefCountBase<T>::value
    inline void safeRelease(T*& obj) {
        if (obj) {
            obj->release();
            obj = nullptr;
        }
    }

#define FCT_REFCOUNT_EXTEND(Derived, UsedByMutilThread) \
    public ::FCT::RefCountExtend<Derived, UsedByMutilThread>

#define FCT_REFCOUNT_EXTEND_CRTP(Derived, UsedByMutilThread) \
    public ::FCT::NewRefCount<Derived, UsedByMutilThread, true>

#define FCT_REFCOUNT_EXTEND_VIRTUAL(Derived, UsedByMutilThread) \
    public ::FCT::NewRefCount<Derived, UsedByMutilThread, false>

#define FCT_EXTEND_REFCOUNT(Derived, UsedByMutilThread) \
    FCT_REFCOUNT_EXTEND(Derived, UsedByMutilThread)

#if FCT_REFCOUNT_USE_CRTP
#define FCT_REFCOUNT_OVERRIDE
#else
#define FCT_REFCOUNT_OVERRIDE override
#endif

#ifndef FCT_SAFE_RELEASE
#define FCT_SAFE_RELEASE(obj) if (obj) { obj->release(); obj = nullptr; }
#endif

#ifndef FCT_SAFE_ADDREF
#define FCT_SAFE_ADDREF(p) if (p) { p->addRef(); }
#endif

    class RefCount {
    public:
        RefCount() : m_refCount(1) {}
        RefCount(size_t refCount) : m_refCount(refCount) {}
        virtual ~RefCount() {}
        void addRef() {
            ++m_refCount;
        }
        void release() {
            if (m_refCount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                deleteThis();
            }
        }
        size_t count() const {
            return m_refCount.load(std::memory_order_relaxed);
        }
        operator bool() const {
            return m_refCount > 0;
        }
    protected:
        virtual void deleteThis() {
            FCT_DELETE(this);
        }
        std::atomic<size_t> m_refCount;
    };

    template<typename T>
    inline void safeAddRef(T* obj) {
        if (obj) obj->addRef();
    }
    template<typename T>
    inline void safeRelease(T*& obj) {
        if (obj) {
            obj->release();
            obj = nullptr;
        }
    }

    template<typename T>
    class Ptr
    {
    public:
        Ptr() noexcept : m_ptr(nullptr) {}
        Ptr(std::nullptr_t) noexcept : m_ptr(nullptr) {}
        Ptr(T* ptr) noexcept : m_ptr(ptr)
        {
            safeAddRef(m_ptr);
        }
        Ptr(const Ptr& other) noexcept : m_ptr(other.m_ptr)
        {
            safeAddRef(m_ptr);
        }
        template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
        Ptr(const Ptr<U>& other) noexcept : m_ptr(other.get())
        {
            safeAddRef(m_ptr);
        }
        Ptr(Ptr&& other) noexcept : m_ptr(other.m_ptr)
        {
            other.m_ptr = nullptr;
        }
        template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
        Ptr(Ptr<U>&& other) noexcept : m_ptr(other.release()) {}

        ~Ptr()
        {
            safeRelease(m_ptr);
        }

        Ptr& operator=(T* ptr) noexcept
        {
            reset(ptr);
            return *this;
        }

        Ptr& operator=(const Ptr& other) noexcept
        {
            if (this != &other)
            {
                T* old = m_ptr;
                m_ptr = other.m_ptr;
                safeAddRef(m_ptr);
                safeRelease(old);
            }
            return *this;
        }

        Ptr& operator=(Ptr&& other) noexcept
        {
            if (this != &other)
            {
                safeRelease(m_ptr);
                m_ptr = other.m_ptr;
                other.m_ptr = nullptr;
            }
            return *this;
        }

        T* get() const noexcept { return m_ptr; }
        T* operator->() const noexcept { return m_ptr; }
        T& operator*() const noexcept { return *m_ptr; }
        explicit operator bool() const noexcept { return m_ptr != nullptr; }

        T* release() noexcept
        {
            T* tmp = m_ptr;
            m_ptr = nullptr;
            return tmp;
        }

        void reset(T* ptr = nullptr) noexcept
        {
            if (m_ptr != ptr)
            {
                T* old = m_ptr;
                m_ptr = ptr;
                safeAddRef(m_ptr);
                safeRelease(old);
            }
        }

    private:
        T* m_ptr{nullptr};
    };

} // namespace FCT

#endif // FCT_BASE_REFCOUNT_H
