#include "./MemoryCheak.h"

namespace FCT {

    namespace {
        std::atomic<MemoryTracker*> g_sharedMemoryTracker{nullptr};
    }

#if defined(FCT_DEBUG) && defined(FCT_USE_MEMORY_CHECK)
    std::vector<_fct_object_t*> fct_object_list;
    std::string fct_object_info;
    int fct_object_count_delete_without_object = 0;
    std::mutex fct_object_mutex;
#endif

    MemoryTracker::MemoryTracker() = default;

    MemoryTracker::~MemoryTracker() {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto* rec : m_ownedRecords) {
            delete rec;
        }
        m_ownedRecords.clear();
        m_pointerToIndex.clear();
#if defined(FCT_DEBUG) && defined(FCT_USE_MEMORY_CHECK)
        if (this == &localInstance()) {
            std::lock_guard<std::mutex> legacyLock(fct_object_mutex);
            fct_object_list.clear();
        }
#endif
    }

    MemoryTracker& MemoryTracker::localInstance() noexcept {
        static MemoryTracker s_localTracker;
        return s_localTracker;
    }

    MemoryTracker& MemoryTracker::instance() noexcept {
        MemoryTracker* shared = g_sharedMemoryTracker.load(std::memory_order_acquire);
        return shared ? *shared : localInstance();
    }

    void MemoryTracker::setSharedInstance(MemoryTracker* shared) noexcept {
        g_sharedMemoryTracker.store(shared, std::memory_order_release);
    }

    void MemoryTracker::updatePeakLocked(size_t tagIndex) noexcept {
        if (m_activeBytes > m_peakBytes) {
            m_peakBytes = m_activeBytes;
        }
        if (tagIndex < m_tagActiveBytes.size() && m_tagActiveBytes[tagIndex] > m_tagPeakBytes[tagIndex]) {
            m_tagPeakBytes[tagIndex] = m_tagActiveBytes[tagIndex];
        }
    }

    void MemoryTracker::recordAllocation(void* ptr, size_t byteSize, std::string describe, MemoryTag tag) {
        if (!ptr) {
            return;
        }

        const size_t tagIdx = static_cast<size_t>(tag) < m_tagActiveBytes.size()
            ? static_cast<size_t>(tag)
            : 0;

        auto* record = new _fct_object_t{};
        record->pointer = ptr;
        record->describe = std::move(describe);
        record->refCounted = -1;
        record->byteSize = byteSize;
        record->tag = tag;

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto existingIt = m_pointerToIndex.find(ptr);
            if (existingIt != m_pointerToIndex.end()) {
                _fct_object_t* oldRec = m_ownedRecords[existingIt->second];
                const size_t oldTagIdx = static_cast<size_t>(oldRec->tag) < m_tagActiveBytes.size()
                    ? static_cast<size_t>(oldRec->tag)
                    : 0;
                if (m_activeBytes >= oldRec->byteSize) {
                    m_activeBytes -= oldRec->byteSize;
                }
                if (m_tagActiveBytes[oldTagIdx] >= oldRec->byteSize) {
                    m_tagActiveBytes[oldTagIdx] -= oldRec->byteSize;
                }
                delete oldRec;
                m_ownedRecords[existingIt->second] = record;
            } else {
                const size_t newIdx = m_ownedRecords.size();
                m_ownedRecords.push_back(record);
                m_pointerToIndex.emplace(ptr, newIdx);
            }

            m_activeBytes += byteSize;
            m_tagActiveBytes[tagIdx] += byteSize;
            ++m_totalAllocations;
            updatePeakLocked(tagIdx);

#if defined(FCT_DEBUG) && defined(FCT_USE_MEMORY_CHECK)
            std::lock_guard<std::mutex> legacyLock(fct_object_mutex);
            fct_object_list = m_ownedRecords;
#endif
        }

#ifdef TRACY_ENABLE
        if (byteSize > 0) {
            TracyAlloc(ptr, byteSize);
        }
#endif
    }

    bool MemoryTracker::recordFree(void* ptr) {
        if (!ptr) {
            return true;
        }

        bool found = false;
        size_t freedBytes = 0;

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_pointerToIndex.find(ptr);
            if (it != m_pointerToIndex.end()) {
                found = true;
                const size_t removeIdx = it->second;
                const size_t lastIdx = m_ownedRecords.size() - 1;
                _fct_object_t* removedRec = m_ownedRecords[removeIdx];
                freedBytes = removedRec->byteSize;

                const size_t tagIdx = static_cast<size_t>(removedRec->tag) < m_tagActiveBytes.size()
                    ? static_cast<size_t>(removedRec->tag)
                    : 0;
                m_activeBytes = (m_activeBytes >= freedBytes) ? (m_activeBytes - freedBytes) : 0;
                m_tagActiveBytes[tagIdx] = (m_tagActiveBytes[tagIdx] >= freedBytes)
                    ? (m_tagActiveBytes[tagIdx] - freedBytes)
                    : 0;
                ++m_totalFrees;

                if (removeIdx != lastIdx) {
                    _fct_object_t* tailRec = m_ownedRecords[lastIdx];
                    m_ownedRecords[removeIdx] = tailRec;
                    m_pointerToIndex[tailRec->pointer] = removeIdx;
                }
                m_ownedRecords.pop_back();
                m_pointerToIndex.erase(it);
                delete removedRec;

#if defined(FCT_DEBUG) && defined(FCT_USE_MEMORY_CHECK)
                std::lock_guard<std::mutex> legacyLock(fct_object_mutex);
                fct_object_list = m_ownedRecords;
#endif
            } else {
                ++m_untrackedFrees;
#if defined(FCT_DEBUG) && defined(FCT_USE_MEMORY_CHECK)
                std::lock_guard<std::mutex> legacyLock(fct_object_mutex);
                ++fct_object_count_delete_without_object;
#endif
            }
        }

#ifdef TRACY_ENABLE
        if (found && freedBytes > 0) {
            TracyFree(ptr);
        }
#endif
        return found;
    }

    void MemoryTracker::recordRawBytesAllocated(size_t byteSize, MemoryTag tag) noexcept {
        if (byteSize == 0) {
            return;
        }
        const size_t tagIdx = static_cast<size_t>(tag) < m_tagActiveBytes.size()
            ? static_cast<size_t>(tag)
            : 0;
        std::lock_guard<std::mutex> lock(m_mutex);
        m_activeBytes += byteSize;
        m_tagActiveBytes[tagIdx] += byteSize;
        ++m_totalAllocations;
        updatePeakLocked(tagIdx);
    }

    void MemoryTracker::recordRawBytesFreed(size_t byteSize, MemoryTag tag) noexcept {
        if (byteSize == 0) {
            return;
        }
        const size_t tagIdx = static_cast<size_t>(tag) < m_tagActiveBytes.size()
            ? static_cast<size_t>(tag)
            : 0;
        std::lock_guard<std::mutex> lock(m_mutex);
        m_activeBytes = (m_activeBytes >= byteSize) ? (m_activeBytes - byteSize) : 0;
        m_tagActiveBytes[tagIdx] = (m_tagActiveBytes[tagIdx] >= byteSize)
            ? (m_tagActiveBytes[tagIdx] - byteSize)
            : 0;
        ++m_totalFrees;
    }

    MemoryStatsSnapshot MemoryTracker::snapshotStats() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        MemoryStatsSnapshot snap;
        snap.activeBytes = m_activeBytes;
        snap.peakBytes = m_peakBytes;
        snap.activeAllocations = m_ownedRecords.size();
        snap.totalAllocations = m_totalAllocations;
        snap.totalFrees = m_totalFrees;
        snap.untrackedFrees = m_untrackedFrees;
        snap.tagActiveBytes = m_tagActiveBytes;
        snap.tagPeakBytes = m_tagPeakBytes;
        return snap;
    }

    std::vector<_fct_object_t*> MemoryTracker::snapshotObjects() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_ownedRecords;
    }

    size_t MemoryTracker::activeAllocationCount() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_ownedRecords.size();
    }

    void MemoryTracker::dumpReport(std::ostream& out) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        out << std::dec;
        out << "=== FCT MemoryTracker 诊断报告 ===" << std::endl;
        out << "活跃字节: " << m_activeBytes << " B | 峰值字节: " << m_peakBytes << " B" << std::endl;
        out << "累计分配: " << m_totalAllocations << " | 累计释放: " << m_totalFrees << std::endl;
        for (size_t i = 0; i < m_tagActiveBytes.size(); ++i) {
            if (m_tagActiveBytes[i] > 0 || m_tagPeakBytes[i] > 0) {
                out << "  [" << getMemoryTagName(static_cast<MemoryTag>(i)) << "] 活跃: "
                    << m_tagActiveBytes[i] << " B, 峰值: " << m_tagPeakBytes[i] << " B" << std::endl;
            }
        }
        for (const auto* rec : m_ownedRecords) {
            out << "  [未释放] " << rec->describe << " (" << rec->byteSize << " B, "
                << getMemoryTagName(rec->tag) << ")" << std::endl;
        }
        out << "不在object表里释放的次数: " << m_untrackedFrees << std::endl;
        out << "未释放对象总计: " << m_ownedRecords.size() << std::endl;
    }

} // namespace FCT
