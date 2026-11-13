#ifndef FCT_MEMORY_CHEAK
#define FCT_MEMORY_CHEAK

#include "./ThirdParty.h"

#ifndef FCT_USE_ANDROID
#define FCT_USE_MEMORY_CHECK
#endif

namespace FCT {

    enum class MemoryTag : uint8_t {
        General = 0,
        RHI = 1,
        Asset = 2,
        Animation = 3,
        Network = 4,
        Script = 5,
        ECS = 6,
        Arena = 7,
        Pool = 8,
        Count = 9
    };

    inline constexpr const char* getMemoryTagName(MemoryTag tag) noexcept {
        switch (tag) {
            case MemoryTag::General:   return "General";
            case MemoryTag::RHI:       return "RHI";
            case MemoryTag::Asset:     return "Asset";
            case MemoryTag::Animation: return "Animation";
            case MemoryTag::Network:   return "Network";
            case MemoryTag::Script:    return "Script";
            case MemoryTag::ECS:       return "ECS";
            case MemoryTag::Arena:     return "Arena";
            case MemoryTag::Pool:      return "Pool";
            default:                   return "Unknown";
        }
    }

    struct MemoryStatsSnapshot {
        size_t activeBytes = 0;
        size_t peakBytes = 0;
        size_t activeAllocations = 0;
        uint64_t totalAllocations = 0;
        uint64_t totalFrees = 0;
        uint64_t untrackedFrees = 0;
        std::array<size_t, static_cast<size_t>(MemoryTag::Count)> tagActiveBytes{};
        std::array<size_t, static_cast<size_t>(MemoryTag::Count)> tagPeakBytes{};
    };

    struct _fct_object_t {
        void* pointer = nullptr;
        std::string describe;
        int refCounted = -1;
        size_t byteSize = 0;
        MemoryTag tag = MemoryTag::General;
    };

    /**
     * @brief 跨模块统一内存分配追踪器，采用哈希索引与尾部交换实现 O(1) 注册与移除，并同步上报 Tracy 内存剖析器
     */
    class MemoryTracker {
    public:
        MemoryTracker();
        ~MemoryTracker();

        MemoryTracker(const MemoryTracker&) = delete;
        MemoryTracker& operator=(const MemoryTracker&) = delete;

        static MemoryTracker& localInstance() noexcept;
        static MemoryTracker& instance() noexcept;
        static void setSharedInstance(MemoryTracker* shared) noexcept;

        void recordAllocation(void* ptr, size_t byteSize, std::string describe, MemoryTag tag = MemoryTag::General);
        bool recordFree(void* ptr);

        void recordRawBytesAllocated(size_t byteSize, MemoryTag tag) noexcept;
        void recordRawBytesFreed(size_t byteSize, MemoryTag tag) noexcept;

        MemoryStatsSnapshot snapshotStats() const;
        std::vector<_fct_object_t*> snapshotObjects() const;
        size_t activeAllocationCount() const;
        void dumpReport(std::ostream& out) const;

    private:
        void updatePeakLocked(size_t tagIndex) noexcept;

        mutable std::mutex m_mutex;
        std::vector<_fct_object_t*> m_ownedRecords;
        std::unordered_map<void*, size_t> m_pointerToIndex;
        size_t m_activeBytes = 0;
        size_t m_peakBytes = 0;
        uint64_t m_totalAllocations = 0;
        uint64_t m_totalFrees = 0;
        uint64_t m_untrackedFrees = 0;
        std::array<size_t, static_cast<size_t>(MemoryTag::Count)> m_tagActiveBytes{};
        std::array<size_t, static_cast<size_t>(MemoryTag::Count)> m_tagPeakBytes{};
    };

#if defined(FCT_DEBUG) && defined(FCT_USE_MEMORY_CHECK)
    extern std::vector<_fct_object_t*> fct_object_list;
    extern std::string fct_object_info;
    extern std::mutex fct_object_mutex;
    extern int fct_object_count_delete_without_object;

    inline void _add_object(void* ptr, const std::string& describe, size_t byteSize = 0, MemoryTag tag = MemoryTag::General) {
        if (!ptr) {
            return;
        }
        MemoryTracker::instance().recordAllocation(ptr, byteSize, describe, tag);
    }

    inline bool _remove_object(void* ptr) {
        if (!ptr) {
            return true;
        }
        return MemoryTracker::instance().recordFree(ptr);
    }

    template <typename T, typename... Args>
    T* _fct_new(Args&&... args) {
        T* ret = new T(std::forward<Args>(args)...);
        _add_object(ret, typeid(T*).name(), sizeof(T), MemoryTag::General);
        return ret;
    }

    template <typename T, typename... Args>
    T* _fct_new_tagged(MemoryTag tag, Args&&... args) {
        T* ret = new T(std::forward<Args>(args)...);
        _add_object(ret, typeid(T*).name(), sizeof(T), tag);
        return ret;
    }

    template <typename T, typename SizeType>
    T* _fct_news(SizeType count) {
        const size_t elemCount = static_cast<size_t>(count);
        T* ret = new T[elemCount];
        _add_object(ret, typeid(T*).name(), sizeof(T) * elemCount, MemoryTag::General);
        return ret;
    }

    template <typename T>
    void _fct_delete(T* ptr) {
        if (!ptr) {
            return;
        }
        _remove_object(static_cast<void*>(const_cast<std::remove_cv_t<T>*>(ptr)));
        delete ptr;
    }

    template <typename T>
    void _fct_deletes(T* ptr) {
        if (!ptr) {
            return;
        }
        _remove_object(static_cast<void*>(const_cast<std::remove_cv_t<T>*>(ptr)));
        delete[] ptr;
    }

    inline void _output_object_nums(std::ostream& out) {
        out << "当前对象总计:" << MemoryTracker::instance().activeAllocationCount() << std::endl;
    }

    inline void _output_object(std::ostream& out) {
        MemoryTracker::instance().dumpReport(out);
    }
#else
    template <typename T, typename... Args>
    T* _fct_new_tagged(MemoryTag tag, Args&&... args) {
        MemoryTracker::instance().recordRawBytesAllocated(sizeof(T), tag);
        return new T(std::forward<Args>(args)...);
    }
#endif

    template<typename T>
    inline void AllocatorDelete(T* ptr) {
        if (!ptr) {
            return;
        }
#if defined(FCT_DEBUG) && defined(FCT_USE_MEMORY_CHECK)
        _remove_object(ptr);
#endif
        operator delete[](ptr);
    }

    template<typename T>
    inline T* AllocatorNew(size_t count, MemoryTag tag = MemoryTag::Pool) {
        const size_t totalBytes = sizeof(T) * count;
        auto* ret = static_cast<T*>(operator new[](totalBytes));
#if defined(FCT_DEBUG) && defined(FCT_USE_MEMORY_CHECK)
        _add_object(
            ret,
            std::string(typeid(T*).name()) + " Chunk Size: " + std::to_string(count),
            totalBytes,
            tag);
#else
        (void)tag;
#endif
        return ret;
    }

    template<typename T>
    inline T* New(T* ptr) {
        return new (ptr) T();
    }

    template<typename T, typename... Args>
    inline T* New(T* ptr, Args&&... args) {
        return new (ptr) T(std::forward<Args>(args)...);
    }

    template<typename T>
    inline void Destroy(T* ptr) {
        if (ptr) {
            ptr->~T();
        }
    }

} // namespace FCT

#if defined(FCT_DEBUG) && defined(FCT_USE_MEMORY_CHECK)
#define FCT_NEW(type, ...) FCT::_fct_new<type>(__VA_ARGS__)
#define FCT_NEW_TAGGED(tag, type, ...) FCT::_fct_new_tagged<type>(tag, __VA_ARGS__)
#define FCT_NEWS(type, num) FCT::_fct_news<type>(num)
#define FCT_DELETE(ptr) FCT::_fct_delete(ptr)
#define FCT_DELETES(ptr) FCT::_fct_deletes(ptr)
#else
#define FCT_NEW(type, ...) new type(__VA_ARGS__)
#define FCT_NEW_TAGGED(tag, type, ...) new type(__VA_ARGS__)
#define FCT_NEWS(type, num) new type[num]
#define FCT_DELETE(ptr) delete (ptr)
#define FCT_DELETES(ptr) delete[] (ptr)
#endif

#define FCT_SAFE_NEW(var, type, ...) \
    do { \
        if (var) { \
            FCT_DELETE(var); \
        } \
        (var) = FCT_NEW(type, __VA_ARGS__); \
    } while (0)

#define FCT_SAFE_DELETE(var) \
    do { \
        if (var) { \
            FCT_DELETE(var); \
            (var) = nullptr; \
        } \
    } while (0)

#endif // FCT_MEMORY_CHEAK
