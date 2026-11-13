#include "./MemoryAllocators.h"

namespace FCT {

    namespace {
        uint8_t* allocateAlignedRaw(size_t byteSize) {
            return static_cast<uint8_t*>(::operator new(byteSize, std::align_val_t{kCacheLineSize}));
        }

        void freeAlignedRaw(uint8_t* ptr) noexcept {
            if (ptr) {
                ::operator delete(ptr, std::align_val_t{kCacheLineSize});
            }
        }
    }

    LinearArena::LinearArena(size_t primaryCapacityBytes, MemoryTag tag)
        : m_primaryCapacity(alignUpSize(primaryCapacityBytes > 0 ? primaryCapacityBytes : 4096, kCacheLineSize))
        , m_tag(tag) {
        m_primaryBlock = allocateAlignedRaw(m_primaryCapacity);
        MemoryTracker::instance().recordRawBytesAllocated(m_primaryCapacity, m_tag);
    }

    LinearArena::~LinearArena() {
        reset();
        if (m_primaryBlock) {
            MemoryTracker::instance().recordRawBytesFreed(m_primaryCapacity, m_tag);
            freeAlignedRaw(m_primaryBlock);
            m_primaryBlock = nullptr;
        }
    }

    void* LinearArena::tryAllocateFromPrimary(size_t byteSize, size_t alignment) noexcept {
        size_t current = m_primaryOffset.load(std::memory_order_relaxed);
        while (true) {
            const uintptr_t baseAddr = reinterpret_cast<uintptr_t>(m_primaryBlock);
            const uintptr_t alignedAddr = alignUpSize(baseAddr + current, alignment);
            const size_t alignedOffset = static_cast<size_t>(alignedAddr - baseAddr);
            const size_t nextOffset = alignedOffset + byteSize;
            if (nextOffset > m_primaryCapacity) {
                return nullptr;
            }
            if (m_primaryOffset.compare_exchange_weak(
                    current, nextOffset, std::memory_order_acq_rel, std::memory_order_relaxed)) {
                recordPeakUsage(nextOffset + m_overflowUsedBytes.load(std::memory_order_relaxed));
                return reinterpret_cast<void*>(alignedAddr);
            }
        }
    }

    void* LinearArena::allocateFromOverflow(size_t byteSize, size_t alignment) {
        std::lock_guard<std::mutex> lock(m_overflowMutex);
        if (!m_overflowBlocks.empty()) {
            OverflowBlock& tail = m_overflowBlocks.back();
            const uintptr_t baseAddr = reinterpret_cast<uintptr_t>(tail.data);
            const uintptr_t alignedAddr = alignUpSize(baseAddr + tail.offset, alignment);
            const size_t alignedOffset = static_cast<size_t>(alignedAddr - baseAddr);
            if (alignedOffset + byteSize <= tail.capacity) {
                const size_t delta = (alignedOffset + byteSize) - tail.offset;
                tail.offset = alignedOffset + byteSize;
                const size_t totalOverflow = m_overflowUsedBytes.fetch_add(delta, std::memory_order_relaxed) + delta;
                recordPeakUsage(m_primaryOffset.load(std::memory_order_relaxed) + totalOverflow);
                return reinterpret_cast<void*>(alignedAddr);
            }
        }

        const size_t minBlockSize = std::max(m_primaryCapacity, alignUpSize(byteSize + alignment, kCacheLineSize));
        OverflowBlock block;
        block.data = allocateAlignedRaw(minBlockSize);
        block.capacity = minBlockSize;
        MemoryTracker::instance().recordRawBytesAllocated(minBlockSize, m_tag);
        m_overflowCapacityBytes.fetch_add(minBlockSize, std::memory_order_relaxed);

        const uintptr_t baseAddr = reinterpret_cast<uintptr_t>(block.data);
        const uintptr_t alignedAddr = alignUpSize(baseAddr, alignment);
        block.offset = static_cast<size_t>(alignedAddr - baseAddr) + byteSize;
        m_overflowBlocks.push_back(block);

        const size_t totalOverflow = m_overflowUsedBytes.fetch_add(block.offset, std::memory_order_relaxed) + block.offset;
        recordPeakUsage(m_primaryOffset.load(std::memory_order_relaxed) + totalOverflow);
        return reinterpret_cast<void*>(alignedAddr);
    }

    void* LinearArena::allocate(size_t byteSize, size_t alignment) {
        const size_t safeSize = byteSize > 0 ? byteSize : 1;
        const size_t safeAlign = alignment > 0 ? alignment : alignof(std::max_align_t);
        if (void* fastPtr = tryAllocateFromPrimary(safeSize, safeAlign)) {
            return fastPtr;
        }
        return allocateFromOverflow(safeSize, safeAlign);
    }

    void LinearArena::registerDestructor(void* target, void (*destroyFn)(void*)) {
        void* nodeMem = allocate(sizeof(DestructorNode), alignof(DestructorNode));
        auto* node = new (nodeMem) DestructorNode{};
        node->destroyFn = destroyFn;
        node->target = target;
        std::lock_guard<std::mutex> lock(m_overflowMutex);
        node->next = m_destructorHead;
        m_destructorHead = node;
    }

    void LinearArena::runDestructorsLocked() noexcept {
        DestructorNode* cur = m_destructorHead;
        m_destructorHead = nullptr;
        while (cur) {
            DestructorNode* next = cur->next;
            if (cur->destroyFn && cur->target) {
                cur->destroyFn(cur->target);
            }
            cur = next;
        }
    }

    void LinearArena::freeOverflowBlocksLocked() noexcept {
        for (auto& block : m_overflowBlocks) {
            if (block.data) {
                MemoryTracker::instance().recordRawBytesFreed(block.capacity, m_tag);
                freeAlignedRaw(block.data);
            }
        }
        m_overflowBlocks.clear();
        m_overflowUsedBytes.store(0, std::memory_order_relaxed);
        m_overflowCapacityBytes.store(0, std::memory_order_relaxed);
    }

    void LinearArena::recordPeakUsage(size_t totalUsed) noexcept {
        size_t prevPeak = m_peakUsedBytes.load(std::memory_order_relaxed);
        while (totalUsed > prevPeak &&
               !m_peakUsedBytes.compare_exchange_weak(
                   prevPeak, totalUsed, std::memory_order_relaxed, std::memory_order_relaxed)) {
        }
    }

    void LinearArena::reset() noexcept {
        {
            std::lock_guard<std::mutex> lock(m_overflowMutex);
            runDestructorsLocked();
            freeOverflowBlocksLocked();
        }
        m_primaryOffset.store(0, std::memory_order_release);
    }

    size_t LinearArena::usedBytes() const noexcept {
        return m_primaryOffset.load(std::memory_order_relaxed) +
               m_overflowUsedBytes.load(std::memory_order_relaxed);
    }

    size_t LinearArena::capacityBytes() const noexcept {
        return m_primaryCapacity + m_overflowCapacityBytes.load(std::memory_order_relaxed);
    }

    DoubleBufferedFrameArena::DoubleBufferedFrameArena(size_t perFrameCapacityBytes) {
        m_arenas[0] = FCT_NEW(LinearArena, perFrameCapacityBytes, MemoryTag::Arena);
        m_arenas[1] = FCT_NEW(LinearArena, perFrameCapacityBytes, MemoryTag::Arena);
    }

    DoubleBufferedFrameArena::~DoubleBufferedFrameArena() {
        FCT_DELETE(m_arenas[0]);
        FCT_DELETE(m_arenas[1]);
        m_arenas[0] = nullptr;
        m_arenas[1] = nullptr;
    }

    void DoubleBufferedFrameArena::nextFrame() noexcept {
        const uint64_t nextIdx = m_frameIndex.fetch_add(1, std::memory_order_acq_rel) + 1;
        m_arenas[nextIdx & 1u]->reset();
    }

    void DoubleBufferedFrameArena::resetAll() noexcept {
        m_arenas[0]->reset();
        m_arenas[1]->reset();
    }

    LinearArena& DoubleBufferedFrameArena::currentArena() noexcept {
        return *m_arenas[m_frameIndex.load(std::memory_order_acquire) & 1u];
    }

    const LinearArena& DoubleBufferedFrameArena::currentArena() const noexcept {
        return *m_arenas[m_frameIndex.load(std::memory_order_acquire) & 1u];
    }

    LinearArena& DoubleBufferedFrameArena::previousArena() noexcept {
        return *m_arenas[(m_frameIndex.load(std::memory_order_acquire) + 1u) & 1u];
    }

    const LinearArena& DoubleBufferedFrameArena::previousArena() const noexcept {
        return *m_arenas[(m_frameIndex.load(std::memory_order_acquire) + 1u) & 1u];
    }

    BlockMemoryPool::BlockMemoryPool(size_t blockSize, size_t blocksPerChunk, MemoryTag tag)
        : m_blockSize(alignUpSize(std::max(blockSize, sizeof(FreeNode)), kCacheLineSize))
        , m_blocksPerChunk(blocksPerChunk > 0 ? blocksPerChunk : 64)
        , m_tag(tag) {
    }

    BlockMemoryPool::~BlockMemoryPool() {
        std::lock_guard<std::mutex> lock(m_mutex);
        const size_t chunkBytes = m_blockSize * m_blocksPerChunk;
        for (uint8_t* chunk : m_chunks) {
            if (chunk) {
                MemoryTracker::instance().recordRawBytesFreed(chunkBytes, m_tag);
                freeAlignedRaw(chunk);
            }
        }
        m_chunks.clear();
        m_freeHead = nullptr;
        m_inUseBlocks = 0;
        m_freeBlocks = 0;
    }

    void BlockMemoryPool::allocateNewChunkLocked() {
        const size_t chunkBytes = m_blockSize * m_blocksPerChunk;
        uint8_t* chunk = allocateAlignedRaw(chunkBytes);
        MemoryTracker::instance().recordRawBytesAllocated(chunkBytes, m_tag);
        m_chunks.push_back(chunk);

        for (size_t i = m_blocksPerChunk; i > 0; --i) {
            auto* node = reinterpret_cast<FreeNode*>(chunk + (i - 1) * m_blockSize);
            node->next = m_freeHead;
            m_freeHead = node;
        }
        m_freeBlocks += m_blocksPerChunk;
    }

    void* BlockMemoryPool::acquire() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_freeHead) {
            allocateNewChunkLocked();
        }
        FreeNode* node = m_freeHead;
        m_freeHead = node->next;
        --m_freeBlocks;
        ++m_inUseBlocks;
        return static_cast<void*>(node);
    }

    void BlockMemoryPool::release(void* block) noexcept {
        if (!block) {
            return;
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        auto* node = static_cast<FreeNode*>(block);
        node->next = m_freeHead;
        m_freeHead = node;
        if (m_inUseBlocks > 0) {
            --m_inUseBlocks;
        }
        ++m_freeBlocks;
    }

    size_t BlockMemoryPool::allocatedBlockCount() const noexcept {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_inUseBlocks;
    }

    size_t BlockMemoryPool::freeBlockCount() const noexcept {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_freeBlocks;
    }

    size_t BlockMemoryPool::totalCapacityBytes() const noexcept {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_chunks.size() * m_blockSize * m_blocksPerChunk;
    }

} // namespace FCT
