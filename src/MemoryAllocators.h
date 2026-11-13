#ifndef FCT_BASE_MEMORY_ALLOCATORS_H
#define FCT_BASE_MEMORY_ALLOCATORS_H

#include "./MemoryCheak.h"

namespace FCT {

    inline constexpr size_t kCacheLineSize = 64;

    inline constexpr size_t alignUpSize(size_t value, size_t alignment) noexcept {
        return (value + (alignment - 1)) & ~(alignment - 1);
    }

    /**
     * @brief 支持多线程无锁主块碰撞分配与溢出链式扩展的线性区域分配器，适用于帧级瞬态内存
     */
    class LinearArena {
    public:
        explicit LinearArena(size_t primaryCapacityBytes = 1024 * 1024, MemoryTag tag = MemoryTag::Arena);
        ~LinearArena();

        LinearArena(const LinearArena&) = delete;
        LinearArena& operator=(const LinearArena&) = delete;

        void* allocate(size_t byteSize, size_t alignment = alignof(std::max_align_t));

        template<typename T>
        T* allocateArray(size_t count) {
            if (count == 0) {
                return nullptr;
            }
            void* mem = allocate(sizeof(T) * count, alignof(T));
            if constexpr (!std::is_trivially_default_constructible_v<T>) {
                T* arr = static_cast<T*>(mem);
                for (size_t i = 0; i < count; ++i) {
                    new (arr + i) T();
                }
                if constexpr (!std::is_trivially_destructible_v<T>) {
                    registerArrayDestructor(arr, count);
                }
                return arr;
            } else {
                return static_cast<T*>(mem);
            }
        }

        template<typename T, typename... Args>
        T* create(Args&&... args) {
            void* mem = allocate(sizeof(T), alignof(T));
            T* obj = new (mem) T(std::forward<Args>(args)...);
            if constexpr (!std::is_trivially_destructible_v<T>) {
                registerDestructor(obj, [](void* p) {
                    static_cast<T*>(p)->~T();
                });
            }
            return obj;
        }

        void reset() noexcept;

        size_t usedBytes() const noexcept;
        size_t capacityBytes() const noexcept;
        size_t peakUsedBytes() const noexcept { return m_peakUsedBytes.load(std::memory_order_relaxed); }

    private:
        struct OverflowBlock {
            uint8_t* data = nullptr;
            size_t capacity = 0;
            size_t offset = 0;
        };

        struct DestructorNode {
            void (*destroyFn)(void*) = nullptr;
            void* target = nullptr;
            DestructorNode* next = nullptr;
        };

        template<typename T>
        void registerArrayDestructor(T* arrayPtr, size_t count) {
            struct ArraySlice {
                T* ptr;
                size_t len;
            };
            void* sliceMem = allocate(sizeof(ArraySlice), alignof(ArraySlice));
            auto* slice = new (sliceMem) ArraySlice{arrayPtr, count};
            registerDestructor(slice, [](void* raw) {
                auto* s = static_cast<ArraySlice*>(raw);
                for (size_t i = s->len; i > 0; --i) {
                    s->ptr[i - 1].~T();
                }
            });
        }

        void* tryAllocateFromPrimary(size_t byteSize, size_t alignment) noexcept;
        void* allocateFromOverflow(size_t byteSize, size_t alignment);
        void registerDestructor(void* target, void (*destroyFn)(void*));
        void runDestructorsLocked() noexcept;
        void freeOverflowBlocksLocked() noexcept;
        void recordPeakUsage(size_t totalUsed) noexcept;

        uint8_t* m_primaryBlock = nullptr;
        size_t m_primaryCapacity = 0;
        std::atomic<size_t> m_primaryOffset{0};
        std::atomic<size_t> m_overflowUsedBytes{0};
        std::atomic<size_t> m_overflowCapacityBytes{0};
        std::atomic<size_t> m_peakUsedBytes{0};
        MemoryTag m_tag = MemoryTag::Arena;

        mutable std::mutex m_overflowMutex;
        std::vector<OverflowBlock> m_overflowBlocks;
        DestructorNode* m_destructorHead = nullptr;
    };

    /**
     * @brief 双缓冲帧分配器，确保跨逻辑帧与渲染同步边界的瞬态数据在相邻帧内安全存活
     */
    class DoubleBufferedFrameArena {
    public:
        explicit DoubleBufferedFrameArena(size_t perFrameCapacityBytes = 2 * 1024 * 1024);
        ~DoubleBufferedFrameArena();

        DoubleBufferedFrameArena(const DoubleBufferedFrameArena&) = delete;
        DoubleBufferedFrameArena& operator=(const DoubleBufferedFrameArena&) = delete;

        void nextFrame() noexcept;
        void resetAll() noexcept;

        LinearArena& currentArena() noexcept;
        const LinearArena& currentArena() const noexcept;
        LinearArena& previousArena() noexcept;
        const LinearArena& previousArena() const noexcept;

        void* allocate(size_t byteSize, size_t alignment = alignof(std::max_align_t)) {
            return currentArena().allocate(byteSize, alignment);
        }

        template<typename T, typename... Args>
        T* create(Args&&... args) {
            return currentArena().create<T>(std::forward<Args>(args)...);
        }

        template<typename T>
        T* allocateArray(size_t count) {
            return currentArena().allocateArray<T>(count);
        }

        uint64_t frameCount() const noexcept { return m_frameIndex.load(std::memory_order_relaxed); }

    private:
        std::array<LinearArena*, 2> m_arenas{nullptr, nullptr};
        std::atomic<uint64_t> m_frameIndex{0};
    };

    /**
     * @brief 侵入式空闲链表的线程安全定长块内存池，适用于网络报文缓冲、骨骼姿态暂存块等高频复用场景
     */
    class BlockMemoryPool {
    public:
        BlockMemoryPool(size_t blockSize, size_t blocksPerChunk = 256, MemoryTag tag = MemoryTag::Pool);
        ~BlockMemoryPool();

        BlockMemoryPool(const BlockMemoryPool&) = delete;
        BlockMemoryPool& operator=(const BlockMemoryPool&) = delete;

        void* acquire();
        void release(void* block) noexcept;

        size_t blockSize() const noexcept { return m_blockSize; }
        size_t allocatedBlockCount() const noexcept;
        size_t freeBlockCount() const noexcept;
        size_t totalCapacityBytes() const noexcept;

    private:
        struct FreeNode {
            FreeNode* next = nullptr;
        };

        void allocateNewChunkLocked();

        size_t m_blockSize = 0;
        size_t m_blocksPerChunk = 0;
        MemoryTag m_tag = MemoryTag::Pool;

        mutable std::mutex m_mutex;
        std::vector<uint8_t*> m_chunks;
        FreeNode* m_freeHead = nullptr;
        size_t m_inUseBlocks = 0;
        size_t m_freeBlocks = 0;
    };

    /**
     * @brief RAII 定长池化缓冲区句柄，析构时自动归还至所属 BlockMemoryPool
     */
    class ScopedPoolBuffer {
    public:
        ScopedPoolBuffer() noexcept = default;
        ScopedPoolBuffer(BlockMemoryPool* pool, void* data, size_t capacity) noexcept
            : m_pool(pool), m_data(static_cast<uint8_t*>(data)), m_capacity(capacity) {}

        ~ScopedPoolBuffer() {
            reset();
        }

        ScopedPoolBuffer(const ScopedPoolBuffer&) = delete;
        ScopedPoolBuffer& operator=(const ScopedPoolBuffer&) = delete;

        ScopedPoolBuffer(ScopedPoolBuffer&& other) noexcept
            : m_pool(other.m_pool), m_data(other.m_data), m_capacity(other.m_capacity) {
            other.m_pool = nullptr;
            other.m_data = nullptr;
            other.m_capacity = 0;
        }

        ScopedPoolBuffer& operator=(ScopedPoolBuffer&& other) noexcept {
            if (this != &other) {
                reset();
                m_pool = other.m_pool;
                m_data = other.m_data;
                m_capacity = other.m_capacity;
                other.m_pool = nullptr;
                other.m_data = nullptr;
                other.m_capacity = 0;
            }
            return *this;
        }

        void reset() noexcept {
            if (m_pool && m_data) {
                m_pool->release(m_data);
            }
            m_pool = nullptr;
            m_data = nullptr;
            m_capacity = 0;
        }

        uint8_t* data() noexcept { return m_data; }
        const uint8_t* data() const noexcept { return m_data; }
        size_t capacity() const noexcept { return m_capacity; }
        explicit operator bool() const noexcept { return m_data != nullptr; }

    private:
        BlockMemoryPool* m_pool = nullptr;
        uint8_t* m_data = nullptr;
        size_t m_capacity = 0;
    };

} // namespace FCT

#endif // FCT_BASE_MEMORY_ALLOCATORS_H
