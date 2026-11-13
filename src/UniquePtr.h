#pragma once

#include <type_traits>
#include <utility>
#include "./MemoryCheak.h"
#include "./RefCount.h"

namespace FCT {

    /**
     * @brief 引擎专属独占所有权智能指针
     * @tparam T 被管理的对象的类型
     * @note
     * - 析构时通过 FCT_DELETE 释放资源并接入引擎内存追踪
     * - 禁用拷贝语义，仅支持移动构造与移动赋值
     * - T 类型不能继承自 FCT::RefCount，引用计数对象应使用 Ptr
     */
    template<typename T>
    class UniquePtr {
    public:
        template<typename U> friend class UniquePtr;

        UniquePtr() noexcept : m_ptr(nullptr) {}

        UniquePtr(std::nullptr_t) noexcept : m_ptr(nullptr) {}

        explicit UniquePtr(T* ptr) noexcept : m_ptr(ptr) {
            static_assert(!std::is_base_of<FCT::RefCount, T>::value && !FCT::IsNewRefCountBase<T>::value,
                "UniquePtr cannot manage types derived from FCT::RefCount. Use a reference-counting pointer instead.");
        }

        UniquePtr(UniquePtr&& other) noexcept : m_ptr(other.release()) {}

        template<typename U, typename = typename std::enable_if<std::is_convertible<U*, T*>::value>::type>
        UniquePtr(UniquePtr<U>&& other) noexcept : m_ptr(other.release()) {}

        UniquePtr& operator=(UniquePtr&& other) noexcept {
            if (this != &other) {
                reset(other.release());
            }
            return *this;
        }

        template<typename U, typename = typename std::enable_if<std::is_convertible<U*, T*>::value>::type>
        UniquePtr& operator=(UniquePtr<U>&& other) noexcept {
            reset(other.release());
            return *this;
        }

        UniquePtr& operator=(std::nullptr_t) noexcept {
            reset();
            return *this;
        }

        UniquePtr(const UniquePtr&) = delete;
        UniquePtr& operator=(const UniquePtr&) = delete;

        ~UniquePtr() {
            reset();
        }

        T* release() noexcept {
            T* temp = m_ptr;
            m_ptr = nullptr;
            return temp;
        }

        void reset(T* ptr = nullptr) noexcept {
            static_assert(!std::is_base_of<FCT::RefCount, T>::value && !FCT::IsNewRefCountBase<T>::value,
                "UniquePtr cannot manage types derived from FCT::RefCount. Use a reference-counting pointer instead.");
            if (m_ptr) {
                FCT_DELETE(m_ptr);
            }
            m_ptr = ptr;
        }

        T* get() const noexcept {
            return m_ptr;
        }

        T& operator*() const {
            return *m_ptr;
        }

        T* operator->() const noexcept {
            return m_ptr;
        }

        explicit operator bool() const noexcept {
            return m_ptr != nullptr;
        }

        bool operator==(std::nullptr_t) const noexcept { return m_ptr == nullptr; }
        bool operator!=(std::nullptr_t) const noexcept { return m_ptr != nullptr; }

    private:
        T* m_ptr;
    };

    /**
     * @brief 创建 UniquePtr 的专属工厂函数，接入 FCT_NEW 内存追踪
     */
    template<typename T, typename... Args>
    UniquePtr<T> makeUnique(Args&&... args) {
        return UniquePtr<T>(FCT_NEW(T, std::forward<Args>(args)...));
    }

} // namespace FCT
