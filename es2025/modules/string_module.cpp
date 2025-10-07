#include "spectre/es2025/modules/string_module.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>

#include "spectre/runtime.h"

namespace spectre::es2025 {
    namespace {
        constexpr std::string_view kName = "String";
        constexpr std::string_view kSummary = "String objects, UTF-16 processing, and text utilities.";
        constexpr std::string_view kReference = "ECMA-262 Section 22.1";

        constexpr std::size_t kHotFrameWindow = 10;
        constexpr std::size_t kInitialTextReserve = 4096;
        constexpr std::size_t kInitialInternSlots = 32;
    }

    StringModule::Metrics::Metrics() noexcept
        : internHits(0),
          internMisses(0),
          reuseHits(0),
          allocations(0),
          releases(0),
          slices(0),
          transforms(0),
          lastFrameTouched(0),
          hotStrings(0),
          activeStrings(0),
          bytesInUse(0),
          bytesReserved(0),
          gpuOptimized(false) {
    }

    StringModule::StringModule()
        : m_Runtime(nullptr),
          m_Subsystems(nullptr),
          m_Config{},
          m_GpuEnabled(false),
          m_Initialized(false),
          m_CurrentFrame(0),
          m_Slots{},
          m_FreeSlots{},
          m_TextStorage{},
          m_InternTable{},
          m_InternCount(0),
          m_FreeBlocks{},
          m_Metrics() {
    }

    std::string_view StringModule::Name() const noexcept {
        return kName;
    }

    std::string_view StringModule::Summary() const noexcept {
        return kSummary;
    }

    std::string_view StringModule::SpecificationReference() const noexcept {
        return kReference;
    }

    void StringModule::Initialize(const ModuleInitContext &context) {
        m_Runtime = &context.runtime;
        m_Subsystems = &context.subsystems;
        m_Config = context.config;
        m_GpuEnabled = context.config.enableGpuAcceleration;
        Reset();
        m_Initialized = true;
    }

    void StringModule::Tick(const TickInfo &info, const ModuleTickContext &) noexcept {
        m_CurrentFrame = info.frameIndex;
        RecomputeHotMetrics();
    }

    void StringModule::OptimizeGpu(const ModuleGpuContext &context) noexcept {
        m_GpuEnabled = context.enableAcceleration;
        m_Metrics.gpuOptimized = m_GpuEnabled;
    }

    void StringModule::Reconfigure(const RuntimeConfig &config) {
        m_Config = config;
        m_GpuEnabled = config.enableGpuAcceleration;
        m_Metrics.gpuOptimized = m_GpuEnabled;
    }

    StatusCode StringModule::Create(std::string_view label, std::string_view value, Handle &outHandle) {
        return CreateInternal(label, value, false, HashValue(value), outHandle);
    }

    StatusCode StringModule::Clone(Handle handle, std::string_view label, Handle &outHandle) {
        const auto *entry = Find(handle);
        if (!entry) {
            outHandle = 0;
            return StatusCode::NotFound;
        }
        auto view = ViewInternal(*entry);
        return CreateInternal(label, view, false, entry->hash, outHandle);
    }

    StatusCode StringModule::Intern(std::string_view value, Handle &outHandle) {
        auto hash = HashValue(value);
        std::uint32_t slotIndex = 0;
        if (LookupIntern(value, hash, slotIndex)) {
            auto &slot = m_Slots[slotIndex];
            if (slot.inUse) {
                slot.entry.refCount += 1;
                Touch(slot.entry);
                outHandle = slot.entry.handle;
                m_Metrics.internHits += 1;
                return StatusCode::Ok;
            }
        }
        EnsureInternCapacity();
        auto status = CreateInternal(value.substr(0, std::min<std::size_t>(64, value.size())),
                                     value, true, hash, outHandle);
        if (status == StatusCode::Ok) {
            m_Metrics.internMisses += 1;
        }
        return status;
    }

    StatusCode StringModule::Release(Handle handle) {
        auto *entry = FindMutable(handle);
        if (!entry) {
            return StatusCode::NotFound;
        }
        if (entry->refCount > 1) {
            entry->refCount -= 1;
            Touch(*entry);
            m_Metrics.releases += 1;
            return StatusCode::Ok;
        }

        auto slotIndex = entry->slot;
        auto capacity = entry->capacity;
        auto length = entry->length;
        auto hash = entry->hash;
        bool pinned = entry->pinned;

        if (capacity > 0) {
            FreeText(entry->offset, capacity);
            if (m_Metrics.bytesReserved >= capacity) {
                m_Metrics.bytesReserved -= capacity;
            } else {
                m_Metrics.bytesReserved = 0;
            }
        }
        if (m_Metrics.bytesInUse >= length) {
            m_Metrics.bytesInUse -= length;
        } else {
            m_Metrics.bytesInUse = 0;
        }

        if (m_Metrics.activeStrings > 0) {
            m_Metrics.activeStrings -= 1;
        }
        m_Metrics.releases += 1;
        m_Metrics.lastFrameTouched = m_CurrentFrame;

        auto &slot = m_Slots[slotIndex];
        slot.inUse = false;
        slot.entry = Entry{};
        slot.entry.slot = slotIndex;
        m_FreeSlots.push_back(slotIndex);

        if (pinned) {
            EraseIntern(slotIndex, hash);
        }
        return StatusCode::Ok;
    }

    StatusCode StringModule::Concat(Handle left, Handle right, std::string_view label, Handle &outHandle) {
        const auto *leftEntry = Find(left);
        const auto *rightEntry = Find(right);
        if (!leftEntry || !rightEntry) {
            outHandle = 0;
            return StatusCode::NotFound;
        }
        auto leftLen = static_cast<std::size_t>(leftEntry->length);
        auto rightLen = static_cast<std::size_t>(rightEntry->length);
        if (leftLen + rightLen > std::numeric_limits<std::uint32_t>::max()) {
            outHandle = 0;
            return StatusCode::CapacityExceeded;
        }
        std::string_view leftView = ViewInternal(*leftEntry);
        std::string_view rightView = ViewInternal(*rightEntry);
        std::string combined;
        combined.reserve(leftLen + rightLen);
        combined.append(leftView);
        combined.append(rightView);
        auto combinedView = std::string_view(combined.data(), combined.size());
        auto status = CreateInternal(label, combinedView, false, HashValue(combinedView), outHandle);
        if (status == StatusCode::Ok) {
            m_Metrics.slices += 1;
        }
        return status;
    }

    StatusCode StringModule::Slice(Handle handle,
                                   std::size_t begin,
                                   std::size_t length,
                                   std::string_view label,
                                   Handle &outHandle) {
        const auto *entry = Find(handle);
        if (!entry) {
            outHandle = 0;
            return StatusCode::NotFound;
        }
        if (begin > entry->length) {
            outHandle = 0;
            return StatusCode::InvalidArgument;
        }
        auto available = static_cast<std::size_t>(entry->length) - begin;
        if (length > available) {
            length = available;
        }
        auto view = ViewInternal(*entry);
        auto slice = view.substr(begin, length);
        auto status = CreateInternal(label, slice, false, HashValue(slice), outHandle);
        if (status == StatusCode::Ok) {
            m_Metrics.slices += 1;
        }
        return status;
    }

    StatusCode StringModule::Append(Handle handle, std::string_view suffix) {
        if (suffix.empty()) {
            return StatusCode::Ok;
        }
        auto *entry = FindMutable(handle);
        if (!entry) {
            return StatusCode::NotFound;
        }
        if (entry->pinned) {
            return StatusCode::InvalidArgument;
        }
        auto required = static_cast<std::size_t>(entry->length) + suffix.size();
        if (required > std::numeric_limits<std::uint32_t>::max()) {
            return StatusCode::CapacityExceeded;
        }
        auto currentView = ViewInternal(*entry);
        if (required > entry->capacity) {
            auto previousCapacity = entry->capacity;
            std::uint32_t newOffset = 0;
            std::uint32_t newCapacity = 0;
            bool reused = AllocateText(required, newOffset, newCapacity);
            WriteText(newOffset, currentView);
            FreeText(entry->offset, entry->capacity);
            entry->offset = newOffset;
            entry->capacity = newCapacity;
            if (m_Metrics.bytesReserved >= previousCapacity) {
                m_Metrics.bytesReserved -= previousCapacity;
            } else {
                m_Metrics.bytesReserved = 0;
            }
            m_Metrics.bytesReserved += newCapacity;
            if (reused) {
                m_Metrics.reuseHits += 1;
            }
        }
        WriteText(entry->offset + entry->length, suffix);
        entry->length = static_cast<std::uint32_t>(required);
        m_Metrics.bytesInUse += suffix.size();
        entry->hash = HashValue(ViewInternal(*entry));
        Touch(*entry);
        m_Metrics.transforms += 1;
        return StatusCode::Ok;
    }

    StatusCode StringModule::ToUpperAscii(Handle handle) noexcept {
        auto *entry = FindMutable(handle);
        if (!entry) {
            return StatusCode::NotFound;
        }
        if (entry->pinned) {
            return StatusCode::InvalidArgument;
        }
        auto *data = reinterpret_cast<unsigned char *>(m_TextStorage.data() + entry->offset);
        bool changed = false;
        for (std::uint32_t i = 0; i < entry->length; ++i) {
            auto c = data[i];
            if (c >= 'a' && c <= 'z') {
                data[i] = static_cast<unsigned char>(c - 32);
                changed = true;
            }
        }
        if (changed) {
            entry->hash = HashValue(ViewInternal(*entry));
            Touch(*entry);
            m_Metrics.transforms += 1;
        }
        return StatusCode::Ok;
    }

    StatusCode StringModule::ToLowerAscii(Handle handle) noexcept {
        auto *entry = FindMutable(handle);
        if (!entry) {
            return StatusCode::NotFound;
        }
        if (entry->pinned) {
            return StatusCode::InvalidArgument;
        }
        auto *data = reinterpret_cast<unsigned char *>(m_TextStorage.data() + entry->offset);
        bool changed = false;
        for (std::uint32_t i = 0; i < entry->length; ++i) {
            auto c = data[i];
            if (c >= 'A' && c <= 'Z') {
                data[i] = static_cast<unsigned char>(c + 32);
                changed = true;
            }
        }
        if (changed) {
            entry->hash = HashValue(ViewInternal(*entry));
            Touch(*entry);
            m_Metrics.transforms += 1;
        }
        return StatusCode::Ok;
    }

    StatusCode StringModule::TrimAscii(Handle handle) noexcept {
        auto *entry = FindMutable(handle);
        if (!entry) {
            return StatusCode::NotFound;
        }
        if (entry->pinned) {
            return StatusCode::InvalidArgument;
        }
        if (entry->length == 0) {
            return StatusCode::Ok;
        }
        auto *data = m_TextStorage.data() + entry->offset;
        std::uint32_t begin = 0;
        std::uint32_t end = entry->length;
        while (begin < end && IsAsciiWhitespace(static_cast<unsigned char>(data[begin]))) {
            ++begin;
        }
        while (end > begin && IsAsciiWhitespace(static_cast<unsigned char>(data[end - 1]))) {
            --end;
        }
        if (begin == 0 && end == entry->length) {
            return StatusCode::Ok;
        }
        auto newLength = end - begin;
        if (begin != 0 && newLength > 0) {
            std::memmove(data, data + begin, newLength);
        }
        if (m_Metrics.bytesInUse >= (entry->length - newLength)) {
            m_Metrics.bytesInUse -= (entry->length - newLength);
        } else {
            m_Metrics.bytesInUse = 0;
        }
        entry->length = newLength;
        entry->hash = HashValue(ViewInternal(*entry));
        Touch(*entry);
        m_Metrics.transforms += 1;
        return StatusCode::Ok;
    }

    bool StringModule::Has(Handle handle) const noexcept {
        return Find(handle) != nullptr;
    }

    std::string_view StringModule::View(Handle handle) const noexcept {
        const auto *entry = Find(handle);
        if (!entry) {
            return {};
        }
        return ViewInternal(*entry);
    }

    std::uint64_t StringModule::Hash(Handle handle) const noexcept {
        const auto *entry = Find(handle);
        return entry ? entry->hash : 0;
    }

    const StringModule::Metrics &StringModule::GetMetrics() const noexcept {
        return m_Metrics;
    }

    bool StringModule::GpuEnabled() const noexcept {
        return m_GpuEnabled;
    }

    StringModule::Entry *StringModule::FindMutable(Handle handle) noexcept {
        if (handle == 0) {
            return nullptr;
        }
        auto slotIndex = DecodeSlot(handle);
        if (slotIndex >= m_Slots.size()) {
            return nullptr;
        }
        auto &slot = m_Slots[slotIndex];
        if (!slot.inUse) {
            return nullptr;
        }
        if (slot.generation != DecodeGeneration(handle)) {
            return nullptr;
        }
        return &slot.entry;
    }

    const StringModule::Entry *StringModule::Find(Handle handle) const noexcept {
        if (handle == 0) {
            return nullptr;
        }
        auto slotIndex = DecodeSlot(handle);
        if (slotIndex >= m_Slots.size()) {
            return nullptr;
        }
        const auto &slot = m_Slots[slotIndex];
        if (!slot.inUse) {
            return nullptr;
        }
        if (slot.generation != DecodeGeneration(handle)) {
            return nullptr;
        }
        return &slot.entry;
    }

    std::uint32_t StringModule::DecodeSlot(Handle handle) noexcept {
        return static_cast<std::uint32_t>(handle & 0xffffffffull);
    }

    std::uint32_t StringModule::DecodeGeneration(Handle handle) noexcept {
        return static_cast<std::uint32_t>((handle >> 32) & 0xffffffffull);
    }

    StringModule::Handle StringModule::EncodeHandle(std::uint32_t slot, std::uint32_t generation) noexcept {
        return (static_cast<Handle>(generation) << 32) | static_cast<Handle>(slot);
    }

    std::uint32_t StringModule::AlignSize(std::size_t size) noexcept {
        constexpr std::size_t alignment = 16;
        auto aligned = (size + (alignment - 1)) & ~(alignment - 1);
        if (aligned > std::numeric_limits<std::uint32_t>::max()) {
            return std::numeric_limits<std::uint32_t>::max();
        }
        return static_cast<std::uint32_t>(aligned);
    }

    bool StringModule::IsAsciiWhitespace(unsigned char c) noexcept {
        return c <= 0x20;
    }

    std::size_t StringModule::NextPowerOfTwo(std::size_t value) noexcept {
        if (value == 0) {
            return 1;
        }
        value -= 1;
        value |= value >> 1;
        value |= value >> 2;
        value |= value >> 4;
        value |= value >> 8;
        value |= value >> 16;
        if constexpr (sizeof(std::size_t) == 8) {
            value |= value >> 32;
        }
        return value + 1;
    }

    void StringModule::Touch(Entry &entry) noexcept {
        entry.version += 1;
        entry.lastTouchFrame = m_CurrentFrame;
        entry.hot = true;
        m_Metrics.lastFrameTouched = m_CurrentFrame;
    }

    void StringModule::Reset() {
        m_Slots.clear();
        m_FreeSlots.clear();
        m_TextStorage.clear();
        m_TextStorage.reserve(kInitialTextReserve);
        m_FreeBlocks.clear();
        m_InternTable.assign(kInitialInternSlots, InternSlot{0, 0, InternState::Empty});
        m_InternCount = 0;
        m_Metrics = Metrics();
        m_Metrics.gpuOptimized = m_GpuEnabled;
        m_CurrentFrame = 0;
    }

    void StringModule::RecomputeHotMetrics() noexcept {
        std::uint64_t hot = 0;
        for (auto &slot: m_Slots) {
            if (!slot.inUse) {
                continue;
            }
            auto &entry = slot.entry;
            entry.hot = (m_CurrentFrame - entry.lastTouchFrame) <= kHotFrameWindow;
            if (entry.hot) {
                ++hot;
            }
        }
        m_Metrics.hotStrings = hot;
        m_Metrics.lastFrameTouched = m_CurrentFrame;
    }

    void StringModule::EnsureTextCapacity(std::size_t requiredAdditionalBytes) {
        if (requiredAdditionalBytes == 0) {
            return;
        }
        auto required = m_TextStorage.size() + requiredAdditionalBytes;
        auto capacity = m_TextStorage.capacity();
        if (capacity >= required) {
            return;
        }
        if (capacity == 0) {
            capacity = kInitialTextReserve;
        }
        while (capacity < required) {
            capacity *= 2;
        }
        m_TextStorage.reserve(capacity);
    }

    bool StringModule::AllocateText(std::size_t length, std::uint32_t &offset, std::uint32_t &capacity) {
        auto aligned = AlignSize(length);
        for (std::size_t i = 0; i < m_FreeBlocks.size(); ++i) {
            auto block = m_FreeBlocks[i];
            if (block.length >= aligned) {
                offset = block.offset;
                capacity = block.length;
                m_FreeBlocks[i] = m_FreeBlocks.back();
                m_FreeBlocks.pop_back();
                return true;
            }
        }
        offset = static_cast<std::uint32_t>(m_TextStorage.size());
        capacity = aligned;
        EnsureTextCapacity(capacity);
        m_TextStorage.resize(static_cast<std::size_t>(offset) + capacity);
        return false;
    }

    void StringModule::FreeText(std::uint32_t offset, std::uint32_t capacity) {
        if (capacity == 0) {
            return;
        }
        FreeBlock freed{offset, capacity};
        for (auto &block: m_FreeBlocks) {
            if (block.offset + block.length == freed.offset) {
                block.length += freed.length;
                return;
            }
            if (freed.offset + freed.length == block.offset) {
                block.offset = freed.offset;
                block.length += freed.length;
                return;
            }
        }
        m_FreeBlocks.push_back(freed);
    }

    void StringModule::WriteText(std::uint32_t offset, std::string_view value) noexcept {
        if (value.empty()) {
            return;
        }
        std::memcpy(m_TextStorage.data() + offset, value.data(), value.size());
    }

    std::string_view StringModule::ViewInternal(const Entry &entry) const noexcept {
        if (entry.length == 0) {
            return {};
        }
        return std::string_view(m_TextStorage.data() + entry.offset, entry.length);
    }

    std::uint64_t StringModule::HashValue(std::string_view value) const noexcept {
        std::uint64_t hash = 1469598103934665603ull;
        for (unsigned char c: value) {
            hash ^= c;
            hash *= 1099511628211ull;
        }
        return hash == 0 ? 1ull : hash;
    }

    StatusCode StringModule::CreateInternal(std::string_view label,
                                            std::string_view value,
                                            bool pinned,
                                            std::uint64_t hash,
                                            Handle &outHandle) {
        outHandle = 0;
        if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
            return StatusCode::CapacityExceeded;
        }
        std::uint32_t offset = 0;
        std::uint32_t capacity = 0;
        bool reused = AllocateText(value.size(), offset, capacity);
        if (reused) {
            m_Metrics.reuseHits += 1;
        }

        std::uint32_t slotIndex = 0;
        std::uint32_t generation = 0;
        if (!m_FreeSlots.empty()) {
            slotIndex = m_FreeSlots.back();
            m_FreeSlots.pop_back();
            auto &slot = m_Slots[slotIndex];
            slot.inUse = true;
            slot.generation += 1;
            generation = slot.generation;
        } else {
            slotIndex = static_cast<std::uint32_t>(m_Slots.size());
            Slot slot{};
            slot.inUse = true;
            slot.generation = 1;
            m_Slots.push_back(slot);
            generation = 1;
        }

        outHandle = EncodeHandle(slotIndex, generation);
        auto &slot = m_Slots[slotIndex];
        slot.inUse = true;
        slot.generation = generation;
        slot.entry.handle = outHandle;
        slot.entry.slot = slotIndex;
        slot.entry.generation = generation;
        slot.entry.offset = offset;
        slot.entry.length = static_cast<std::uint32_t>(value.size());
        slot.entry.capacity = capacity;
        slot.entry.hash = hash;
        slot.entry.refCount = 1;
        slot.entry.label.assign(label);
        slot.entry.version = 0;
        slot.entry.lastTouchFrame = m_CurrentFrame;
        slot.entry.hot = true;
        slot.entry.pinned = pinned;

        WriteText(offset, value);

        m_Metrics.allocations += 1;
        m_Metrics.activeStrings += 1;
        m_Metrics.bytesInUse += value.size();
        m_Metrics.bytesReserved += capacity;
        m_Metrics.lastFrameTouched = m_CurrentFrame;

        Touch(slot.entry);

        if (pinned) {
            InsertIntern(slotIndex, hash);
        }
        return StatusCode::Ok;
    }

    void StringModule::EnsureInternCapacity() {
        if (m_InternTable.empty()) {
            m_InternTable.assign(kInitialInternSlots, InternSlot{0, 0, InternState::Empty});
            m_InternCount = 0;
            return;
        }
        auto capacity = m_InternTable.size();
        if ((m_InternCount + 1) * 10 <= capacity * 6) {
            return;
        }
        auto newCapacity = NextPowerOfTwo(capacity * 2);
        std::vector<InternSlot> newTable(newCapacity, InternSlot{0, 0, InternState::Empty});
        auto oldTable = std::move(m_InternTable);
        m_InternTable = std::move(newTable);
        auto previousCount = m_InternCount;
        m_InternCount = 0;
        for (const auto &slot: oldTable) {
            if (slot.state == InternState::Occupied) {
                InsertIntern(slot.slot, slot.hash);
            }
        }
        (void) previousCount;
    }

    bool StringModule::LookupIntern(std::string_view value, std::uint64_t hash, std::uint32_t &outSlot) const noexcept {
        if (m_InternTable.empty()) {
            return false;
        }
        auto mask = m_InternTable.size() - 1;
        auto index = static_cast<std::size_t>(hash) & mask;
        for (std::size_t probe = 0; probe < m_InternTable.size(); ++probe) {
            const auto &entry = m_InternTable[index];
            if (entry.state == InternState::Empty) {
                return false;
            }
            if (entry.state == InternState::Occupied && entry.hash == hash) {
                if (entry.slot < m_Slots.size()) {
                    const auto &slot = m_Slots[entry.slot];
                    if (slot.inUse) {
                        auto view = ViewInternal(slot.entry);
                        if (view.size() == value.size() && std::memcmp(view.data(), value.data(), value.size()) == 0) {
                            outSlot = entry.slot;
                            return true;
                        }
                    }
                }
            }
            index = (index + 1) & mask;
        }
        return false;
    }

    void StringModule::InsertIntern(std::uint32_t slotIndex, std::uint64_t hash) {
        if (m_InternTable.empty()) {
            m_InternTable.assign(kInitialInternSlots, InternSlot{0, 0, InternState::Empty});
        }
        auto mask = m_InternTable.size() - 1;
        auto index = static_cast<std::size_t>(hash) & mask;
        while (true) {
            auto &entry = m_InternTable[index];
            if (entry.state == InternState::Empty || entry.state == InternState::Tombstone) {
                entry.hash = hash;
                entry.slot = slotIndex;
                entry.state = InternState::Occupied;
                m_InternCount += 1;
                return;
            }
            index = (index + 1) & mask;
        }
    }

    void StringModule::EraseIntern(std::uint32_t slotIndex, std::uint64_t hash) {
        if (m_InternTable.empty()) {
            return;
        }
        auto mask = m_InternTable.size() - 1;
        auto index = static_cast<std::size_t>(hash) & mask;
        for (std::size_t probe = 0; probe < m_InternTable.size(); ++probe) {
            auto &entry = m_InternTable[index];
            if (entry.state == InternState::Empty) {
                return;
            }
            if (entry.state == InternState::Occupied && entry.hash == hash && entry.slot == slotIndex) {
                entry.state = InternState::Tombstone;
                if (m_InternCount > 0) {
                    m_InternCount -= 1;
                }
                return;
            }
            index = (index + 1) & mask;
        }
    }
}