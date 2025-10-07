#include "spectre/es2025/modules/bigint_module.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <sstream>

#include "spectre/runtime.h"

namespace spectre::es2025 {
    namespace {
        constexpr std::string_view kName = "BigInt";
        constexpr std::string_view kSummary = "BigInt primitive integration and big integer arithmetic.";
        constexpr std::string_view kReference = "ECMA-262 Section 20.2";
        constexpr std::size_t kMaxDecimalDigits = 4096;
        constexpr std::uint32_t kDecimalChunk = 1000000000u; // 1e9
    }

    BigIntModule::Metrics::Metrics() noexcept
        : canonicalHits(0),
          canonicalMisses(0),
          allocations(0),
          releases(0),
          additions(0),
          multiplications(0),
          conversions(0),
          normalizedDigits(0),
          hotIntegers(0),
          lastFrameTouched(0),
          maxDigits(0),
          gpuOptimized(false) {
    }

    BigIntModule::BigIntModule()
        : m_Runtime(nullptr),
          m_Subsystems(nullptr),
          m_Config{},
          m_GpuEnabled(false),
          m_Initialized(false),
          m_CurrentFrame(0),
          m_Slots{},
          m_FreeSlots{},
          m_CanonicalZero(0),
          m_CanonicalOne(0),
          m_CanonicalMinusOne(0),
          m_Metrics() {
    }

    std::string_view BigIntModule::Name() const noexcept {
        return kName;
    }

    std::string_view BigIntModule::Summary() const noexcept {
        return kSummary;
    }

    std::string_view BigIntModule::SpecificationReference() const noexcept {
        return kReference;
    }

    void BigIntModule::Initialize(const ModuleInitContext &context) {
        m_Runtime = &context.runtime;
        m_Subsystems = &context.subsystems;
        m_Config = context.config;
        m_GpuEnabled = context.config.enableGpuAcceleration;
        Reset();
        m_Initialized = true;
    }

    void BigIntModule::Tick(const TickInfo &info, const ModuleTickContext &) noexcept {
        m_CurrentFrame = info.frameIndex;
        RecomputeHotMetrics();
    }

    void BigIntModule::OptimizeGpu(const ModuleGpuContext &context) noexcept {
        m_GpuEnabled = context.enableAcceleration;
        m_Metrics.gpuOptimized = m_GpuEnabled;
    }

    void BigIntModule::Reconfigure(const RuntimeConfig &config) {
        m_Config = config;
        m_GpuEnabled = config.enableGpuAcceleration;
        m_Metrics.gpuOptimized = m_GpuEnabled;
    }

    StatusCode BigIntModule::Create(std::string_view label, std::int64_t value, Handle &outHandle) {
        return CreateInternal(label, value, false, outHandle);
    }

    StatusCode BigIntModule::CreateFromDecimal(std::string_view label, std::string_view text, Handle &outHandle) {
        outHandle = 0;
        if (text.size() > kMaxDecimalDigits) {
            return StatusCode::InvalidArgument;
        }
        std::size_t index = 0;
        while (index < text.size() && std::isspace(static_cast<unsigned char>(text[index])) != 0) {
            ++index;
        }
        bool negative = false;
        if (index < text.size() && (text[index] == '+' || text[index] == '-')) {
            negative = (text[index] == '-');
            ++index;
        }
        std::vector<std::uint32_t> limbs{0};
        bool anyDigit = false;
        for (; index < text.size(); ++index) {
            unsigned char c = static_cast<unsigned char>(text[index]);
            if (std::isspace(c) != 0) {
                break;
            }
            if (c < '0' || c > '9') {
                return StatusCode::InvalidArgument;
            }
            std::uint32_t digit = static_cast<std::uint32_t>(c - '0');
            std::uint64_t carry = 0;
            for (auto &limb: limbs) {
                std::uint64_t value = static_cast<std::uint64_t>(limb) * 10ull + carry;
                limb = static_cast<std::uint32_t>(value & 0xffffffffull);
                carry = value >> 32;
            }
            if (carry != 0) {
                limbs.push_back(static_cast<std::uint32_t>(carry));
            }
            std::uint64_t addCarry = digit;
            for (std::size_t i = 0; i < limbs.size(); ++i) {
                std::uint64_t value = static_cast<std::uint64_t>(limbs[i]) + addCarry;
                limbs[i] = static_cast<std::uint32_t>(value & 0xffffffffull);
                addCarry = value >> 32;
                if (addCarry == 0) {
                    break;
                }
            }
            if (addCarry != 0) {
                limbs.push_back(static_cast<std::uint32_t>(addCarry));
            }
            anyDigit = true;
        }
        if (!anyDigit) {
            negative = false;
        }
        while (index < text.size() && std::isspace(static_cast<unsigned char>(text[index])) != 0) {
            ++index;
        }
        if (index != text.size()) {
            return StatusCode::InvalidArgument;
        }
        if (limbs.size() == 1 && limbs.front() == 0) {
            limbs.clear();
            negative = false;
        }
        return CreateFromLimbs(label, limbs, negative, false, outHandle);
    }

    StatusCode BigIntModule::Clone(Handle handle, std::string_view label, Handle &outHandle) {
        const auto *entry = Find(handle);
        if (!entry) {
            outHandle = 0;
            return StatusCode::NotFound;
        }
        return CreateFromLimbs(label, entry->limbs, entry->negative, false, outHandle);
    }

    StatusCode BigIntModule::Destroy(Handle handle) {
        auto *entry = FindMutable(handle);
        if (!entry) {
            return StatusCode::NotFound;
        }
        if (entry->pinned) {
            return StatusCode::InvalidArgument;
        }
        auto slotIndex = entry->slot;
        m_Slots[slotIndex].inUse = false;
        m_Slots[slotIndex].entry = Entry{};
        m_Slots[slotIndex].entry.slot = slotIndex;
        m_FreeSlots.push_back(slotIndex);
        m_Metrics.releases += 1;
        return StatusCode::Ok;
    }

    StatusCode BigIntModule::Add(Handle targetHandle, Handle addendHandle) {
        auto *target = FindMutable(targetHandle);
        const auto *addend = Find(addendHandle);
        if (!target || !addend) {
            return StatusCode::NotFound;
        }
        if (target->pinned) {
            return StatusCode::InvalidArgument;
        }
        if (addend->limbs.empty()) {
            return StatusCode::Ok;
        }
        if (target->negative == addend->negative) {
            AddLimbs(*target, *addend);
        } else {
            int cmp = CompareAbs(*target, *addend);
            if (cmp == 0) {
                target->limbs.clear();
                target->negative = false;
            } else if (cmp > 0) {
                SubtractLimbs(*target, *addend);
            } else {
                Entry temp;
                temp.limbs = addend->limbs;
                temp.negative = addend->negative;
                SubtractLimbs(temp, *target);
                target->limbs.swap(temp.limbs);
                target->negative = temp.negative;
            }
        }
        Normalize(*target);
        Touch(*target);
        m_Metrics.additions += 1;
        return StatusCode::Ok;
    }

    StatusCode BigIntModule::AddSigned(Handle handle, std::int64_t delta) {
        auto *entry = FindMutable(handle);
        if (!entry) {
            return StatusCode::NotFound;
        }
        if (entry->pinned) {
            return StatusCode::InvalidArgument;
        }
        AddSignedSmall(*entry, delta);
        Normalize(*entry);
        Touch(*entry);
        m_Metrics.additions += 1;
        return StatusCode::Ok;
    }

    StatusCode BigIntModule::MultiplySmall(Handle handle, std::uint32_t factor) {
        auto *entry = FindMutable(handle);
        if (!entry) {
            return StatusCode::NotFound;
        }
        if (entry->pinned) {
            return StatusCode::InvalidArgument;
        }
        if (entry->limbs.empty()) {
            return StatusCode::Ok;
        }
        if (factor == 0) {
            entry->limbs.clear();
            entry->negative = false;
            Normalize(*entry);
            Touch(*entry);
            m_Metrics.multiplications += 1;
            return StatusCode::Ok;
        }
        if (factor == 1) {
            return StatusCode::Ok;
        }
        MultiplySmallInternal(*entry, factor);
        Normalize(*entry);
        Touch(*entry);
        m_Metrics.multiplications += 1;
        return StatusCode::Ok;
    }

    StatusCode BigIntModule::ShiftLeft(Handle handle, std::size_t bits) {
        if (bits == 0) {
            return StatusCode::Ok;
        }
        auto *entry = FindMutable(handle);
        if (!entry) {
            return StatusCode::NotFound;
        }
        if (entry->pinned) {
            return StatusCode::InvalidArgument;
        }
        ShiftLeftInternal(*entry, bits);
        Normalize(*entry);
        Touch(*entry);
        m_Metrics.multiplications += 1;
        return StatusCode::Ok;
    }

    BigIntModule::ComparisonResult BigIntModule::Compare(Handle leftHandle, Handle rightHandle) const noexcept {
        const auto *left = Find(leftHandle);
        const auto *right = Find(rightHandle);
        if (!left || !right) {
            return {0, 0};
        }
        int cmp = CompareInternal(*left, *right);
        auto digits = std::max(left->limbs.size(), right->limbs.size());
        return {cmp, digits};
    }

    bool BigIntModule::IsZero(Handle handle) const noexcept {
        const auto *entry = Find(handle);
        return !entry || entry->limbs.empty();
    }

    StatusCode BigIntModule::ToDecimalString(Handle handle, std::string &outText) const {
        const auto *entry = Find(handle);
        if (!entry) {
            outText.clear();
            return StatusCode::NotFound;
        }
        DecimalFromEntry(*entry, outText);
        m_Metrics.conversions += 1;
        return StatusCode::Ok;
    }

    StatusCode BigIntModule::ToUint64(Handle handle, std::uint64_t &outValue) const noexcept {
        const auto *entry = Find(handle);
        if (!entry) {
            outValue = 0;
            return StatusCode::NotFound;
        }
        if (entry->negative) {
            return StatusCode::InvalidArgument;
        }
        std::uint64_t value = 0;
        std::size_t maxLimbs = std::min<std::size_t>(entry->limbs.size(), 2);
        for (std::size_t i = 0; i < maxLimbs; ++i) {
            value |= static_cast<std::uint64_t>(entry->limbs[i]) << (i * kLimbBits);
        }
        if (entry->limbs.size() > 2) {
            return StatusCode::CapacityExceeded;
        }
        outValue = value;
        m_Metrics.conversions += 1;
        return StatusCode::Ok;
    }

    BigIntModule::Handle BigIntModule::Canonical(std::int64_t value) {
        if (value == 0) {
            m_Metrics.canonicalHits += 1;
            return m_CanonicalZero;
        }
        if (value == 1) {
            m_Metrics.canonicalHits += 1;
            return m_CanonicalOne;
        }
        if (value == -1) {
            m_Metrics.canonicalHits += 1;
            return m_CanonicalMinusOne;
        }
        m_Metrics.canonicalMisses += 1;
        Handle handle = 0;
        CreateInternal("bigint.fast", value, true, handle);
        return handle;
    }

    const BigIntModule::Metrics &BigIntModule::GetMetrics() const noexcept {
        return m_Metrics;
    }

    bool BigIntModule::GpuEnabled() const noexcept {
        return m_GpuEnabled;
    }

    BigIntModule::Entry *BigIntModule::FindMutable(Handle handle) noexcept {
        if (handle == 0) {
            return nullptr;
        }
        auto slotIndex = DecodeSlot(handle);
        if (slotIndex >= m_Slots.size()) {
            return nullptr;
        }
        auto &slot = m_Slots[slotIndex];
        if (!slot.inUse || slot.generation != DecodeGeneration(handle)) {
            return nullptr;
        }
        return &slot.entry;
    }

    const BigIntModule::Entry *BigIntModule::Find(Handle handle) const noexcept {
        if (handle == 0) {
            return nullptr;
        }
        auto slotIndex = DecodeSlot(handle);
        if (slotIndex >= m_Slots.size()) {
            return nullptr;
        }
        const auto &slot = m_Slots[slotIndex];
        if (!slot.inUse || slot.generation != DecodeGeneration(handle)) {
            return nullptr;
        }
        return &slot.entry;
    }

    std::uint32_t BigIntModule::DecodeSlot(Handle handle) noexcept {
        return static_cast<std::uint32_t>(handle & 0xffffffffull);
    }

    std::uint32_t BigIntModule::DecodeGeneration(Handle handle) noexcept {
        return static_cast<std::uint32_t>((handle >> 32) & 0xffffffffull);
    }

    BigIntModule::Handle BigIntModule::EncodeHandle(std::uint32_t slot, std::uint32_t generation) noexcept {
        return (static_cast<Handle>(generation) << 32) | static_cast<Handle>(slot);
    }

    StatusCode BigIntModule::CreateInternal(std::string_view label, std::int64_t value, bool pinned,
                                            Handle &outHandle) {
        std::vector<std::uint32_t> limbs;
        if (value != 0) {
            std::uint64_t magnitude = static_cast<std::uint64_t>(value < 0 ? -value : value);
            while (magnitude != 0) {
                limbs.push_back(static_cast<std::uint32_t>(magnitude & 0xffffffffull));
                magnitude >>= kLimbBits;
            }
        }
        return CreateFromLimbs(label, limbs, value < 0, pinned, outHandle);
    }

    StatusCode BigIntModule::CreateFromLimbs(std::string_view label,
                                             const std::vector<std::uint32_t> &limbs,
                                             bool negative,
                                             bool pinned,
                                             Handle &outHandle) {
        std::uint32_t slotIndex = 0;
        if (!m_FreeSlots.empty()) {
            slotIndex = m_FreeSlots.back();
            m_FreeSlots.pop_back();
            auto &slot = m_Slots[slotIndex];
            slot.inUse = true;
            slot.generation += 1;
        } else {
            slotIndex = static_cast<std::uint32_t>(m_Slots.size());
            Slot slot{};
            slot.inUse = true;
            slot.generation = 1;
            m_Slots.push_back(slot);
        }
        auto &slot = m_Slots[slotIndex];
        slot.entry.handle = EncodeHandle(slotIndex, slot.generation);
        slot.entry.slot = slotIndex;
        slot.entry.generation = slot.generation;
        slot.entry.negative = negative && !limbs.empty();
        slot.entry.limbs = limbs;
        slot.entry.label.assign(label);
        slot.entry.version = 0;
        slot.entry.lastTouchFrame = m_CurrentFrame;
        slot.entry.hot = true;
        slot.entry.pinned = pinned;
        Normalize(slot.entry);
        outHandle = slot.entry.handle;
        Touch(slot.entry);
        if (!pinned) {
            m_Metrics.allocations += 1;
        }
        return StatusCode::Ok;
    }

    void BigIntModule::Reset() {
        m_Slots.clear();
        m_FreeSlots.clear();
        m_CurrentFrame = 0;
        m_Metrics = Metrics();
        m_Metrics.gpuOptimized = m_GpuEnabled;
        Handle zero = 0;
        Handle one = 0;
        Handle minusOne = 0;
        CreateInternal("bigint.zero", 0, true, zero);
        CreateInternal("bigint.one", 1, true, one);
        CreateInternal("bigint.minus_one", -1, true, minusOne);
        m_CanonicalZero = zero;
        m_CanonicalOne = one;
        m_CanonicalMinusOne = minusOne;
    }

    void BigIntModule::Normalize(Entry &entry) noexcept {
        auto &limbs = entry.limbs;
        while (!limbs.empty() && limbs.back() == 0) {
            limbs.pop_back();
        }
        if (limbs.empty()) {
            entry.negative = false;
        }
        ObserveDigits(limbs.size());
        m_Metrics.normalizedDigits += limbs.size();
    }

    void BigIntModule::Touch(Entry &entry) noexcept {
        entry.version += 1;
        entry.lastTouchFrame = m_CurrentFrame;
        entry.hot = true;
        m_Metrics.lastFrameTouched = m_CurrentFrame;
    }

    void BigIntModule::RecomputeHotMetrics() noexcept {
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
        m_Metrics.hotIntegers = hot;
        m_Metrics.lastFrameTouched = m_CurrentFrame;
    }

    void BigIntModule::AddLimbs(Entry &target, const Entry &source) {
        auto &dst = target.limbs;
        const auto &src = source.limbs;
        std::size_t maxSize = std::max(dst.size(), src.size());
        dst.resize(maxSize, 0);
        std::uint64_t carry = 0;
        for (std::size_t i = 0; i < maxSize; ++i) {
            std::uint64_t sum = static_cast<std::uint64_t>(dst[i]) +
                                (i < src.size() ? static_cast<std::uint64_t>(src[i]) : 0ull) + carry;
            dst[i] = static_cast<std::uint32_t>(sum & 0xffffffffull);
            carry = sum >> 32;
        }
        if (carry != 0) {
            dst.push_back(static_cast<std::uint32_t>(carry));
        }
    }

    void BigIntModule::SubtractLimbs(Entry &target, const Entry &source) {
        auto &dst = target.limbs;
        const auto &src = source.limbs;
        std::uint64_t borrow = 0;
        for (std::size_t i = 0; i < dst.size(); ++i) {
            std::uint64_t a = static_cast<std::uint64_t>(dst[i]);
            std::uint64_t b = (i < src.size() ? static_cast<std::uint64_t>(src[i]) : 0ull) + borrow;
            if (a >= b) {
                dst[i] = static_cast<std::uint32_t>(a - b);
                borrow = 0;
            } else {
                dst[i] = static_cast<std::uint32_t>((a + kLimbBase) - b);
                borrow = 1;
            }
        }
    }

    int BigIntModule::CompareAbs(const Entry &left, const Entry &right) const noexcept {
        if (left.limbs.size() != right.limbs.size()) {
            return left.limbs.size() > right.limbs.size() ? 1 : -1;
        }
        for (std::size_t i = left.limbs.size(); i-- > 0;) {
            if (left.limbs[i] != right.limbs[i]) {
                return left.limbs[i] > right.limbs[i] ? 1 : -1;
            }
        }
        return 0;
    }

    void BigIntModule::AddSignedSmall(Entry &target, std::int64_t delta) {
        if (delta == 0) {
            return;
        }
        if (target.limbs.empty()) {
            target.negative = delta < 0;
            std::uint64_t magnitude = static_cast<std::uint64_t>(delta < 0 ? -delta : delta);
            target.limbs.clear();
            while (magnitude != 0) {
                target.limbs.push_back(static_cast<std::uint32_t>(magnitude & 0xffffffffull));
                magnitude >>= 32;
            }
            return;
        }
        if ((delta < 0) == target.negative) {
            std::uint64_t magnitude = static_cast<std::uint64_t>(delta < 0 ? -delta : delta);
            std::uint64_t carry = magnitude;
            std::size_t i = 0;
            while (carry != 0 && i < target.limbs.size()) {
                std::uint64_t sum = static_cast<std::uint64_t>(target.limbs[i]) + (carry & 0xffffffffull);
                target.limbs[i] = static_cast<std::uint32_t>(sum & 0xffffffffull);
                carry = (carry >> 32) + (sum >> 32);
                ++i;
            }
            while (carry != 0) {
                target.limbs.push_back(static_cast<std::uint32_t>(carry & 0xffffffffull));
                carry >>= 32;
            }
        } else {
            std::uint64_t magnitude = static_cast<std::uint64_t>(delta < 0 ? -delta : delta);
            Entry temp;
            temp.limbs.clear();
            while (magnitude != 0) {
                temp.limbs.push_back(static_cast<std::uint32_t>(magnitude & 0xffffffffull));
                magnitude >>= 32;
            }
            temp.negative = delta < 0;
            int cmp = CompareAbs(target, temp);
            if (cmp == 0) {
                target.limbs.clear();
                target.negative = false;
                return;
            }
            if ((cmp > 0 && !target.negative) || (cmp < 0 && target.negative)) {
                SubtractLimbs(target, temp);
            } else {
                SubtractLimbs(temp, target);
                target.limbs.swap(temp.limbs);
                target.negative = delta < 0;
            }
        }
    }

    void BigIntModule::MultiplySmallInternal(Entry &entry, std::uint32_t factor) {
        std::uint64_t carry = 0;
        for (auto &limb: entry.limbs) {
            std::uint64_t value = static_cast<std::uint64_t>(limb) * factor + carry;
            limb = static_cast<std::uint32_t>(value & 0xffffffffull);
            carry = value >> 32;
        }
        if (carry != 0) {
            entry.limbs.push_back(static_cast<std::uint32_t>(carry));
        }
    }

    void BigIntModule::ShiftLeftInternal(Entry &entry, std::size_t bits) {
        if (entry.limbs.empty()) {
            return;
        }
        std::size_t limbShift = bits / kLimbBits;
        std::uint32_t bitShift = static_cast<std::uint32_t>(bits % kLimbBits);
        if (limbShift != 0) {
            entry.limbs.insert(entry.limbs.begin(), limbShift, 0u);
        }
        if (bitShift == 0) {
            return;
        }
        std::uint32_t carry = 0;
        for (auto &limb: entry.limbs) {
            std::uint64_t value = (static_cast<std::uint64_t>(limb) << bitShift) | carry;
            limb = static_cast<std::uint32_t>(value & 0xffffffffull);
            carry = static_cast<std::uint32_t>(value >> 32);
        }
        if (carry != 0) {
            entry.limbs.push_back(carry);
        }
    }

    int BigIntModule::CompareInternal(const Entry &left, const Entry &right) const noexcept {
        if (left.negative != right.negative) {
            return left.negative ? -1 : 1;
        }
        int cmp = CompareAbs(left, right);
        return left.negative ? -cmp : cmp;
    }

    void BigIntModule::DecimalFromEntry(const Entry &entry, std::string &outText) const {
        if (entry.limbs.empty()) {
            outText = "0";
            return;
        }
        std::vector<std::uint32_t> scratch = entry.limbs;
        std::vector<std::uint32_t> chunks;
        chunks.reserve(scratch.size() * 2);
        while (!scratch.empty()) {
            std::uint64_t remainder = 0;
            for (std::size_t i = scratch.size(); i-- > 0;) {
                std::uint64_t value = (remainder << 32) | scratch[i];
                scratch[i] = static_cast<std::uint32_t>(value / kDecimalChunk);
                remainder = value % kDecimalChunk;
            }
            chunks.push_back(static_cast<std::uint32_t>(remainder));
            while (!scratch.empty() && scratch.back() == 0) {
                scratch.pop_back();
            }
        }
        std::ostringstream stream;
        if (entry.negative) {
            stream << '-';
        }
        auto iter = chunks.rbegin();
        stream << *iter;
        ++iter;
        for (; iter != chunks.rend(); ++iter) {
            stream.width(9);
            stream.fill('0');
            stream << *iter;
        }
        outText = stream.str();
    }

    void BigIntModule::ObserveDigits(std::size_t digits) noexcept {
        m_Metrics.maxDigits = std::max(m_Metrics.maxDigits, static_cast<std::uint64_t>(digits));
    }
}