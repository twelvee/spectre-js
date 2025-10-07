#include "spectre/es2025/modules/date_module.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include "spectre/runtime.h"

namespace spectre::es2025 {
    namespace {
        constexpr std::string_view kName = "Date";
        constexpr std::string_view kSummary = "Date objects, calendar math, and host time access.";
        constexpr std::string_view kReference = "ECMA-262 Section 21.4";

        constexpr std::int64_t kMillisPerSecond = 1000;
        constexpr std::int64_t kMillisPerMinute = kMillisPerSecond * 60;
        constexpr std::int64_t kMillisPerHour = kMillisPerMinute * 60;
        constexpr std::int64_t kMillisPerDay = kMillisPerHour * 24;
        constexpr int kDaysPerWeek = 7;

        constexpr int kMonthOffsets[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
        constexpr int kMonthOffsetsLeap[12] = {0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335};

        inline bool IsLeap(int year) noexcept {
            return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
        }

        inline int DaysInMonth(int year, int month) noexcept {
            static constexpr int kMonthDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
            if (month == 2 && IsLeap(year)) {
                return 29;
            }
            return kMonthDays[month - 1];
        }

        std::int64_t DaysFromCivil(int year, int month, int day) noexcept {
            year -= month <= 2;
            const int era = (year >= 0 ? year : year - 399) / 400;
            const unsigned yoe = static_cast<unsigned>(year - era * 400);
            const unsigned doy = (153u * (static_cast<unsigned>(month + (month > 2 ? -3 : 9))) + 2u) / 5u + static_cast<unsigned>(day) - 1u;
            const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + yoe / 400u + doy;
            return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
        }

        void CivilFromDays(std::int64_t days, int &year, unsigned &month, unsigned &day) noexcept {
            days += 719468;
            const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
            const unsigned doe = static_cast<unsigned>(days - era * 146097);
            const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
            const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100 + yoe / 400);
            const unsigned mp = (5 * doy + 2) / 153;
            day = doy - (153 * mp + 2) / 5 + 1;
            month = mp + (mp < 10 ? 3 : -9);
            year = static_cast<int>(yoe) + static_cast<int>(era) * 400 + (month <= 2 ? 1 : 0);
        }

        inline void SplitDaysAndMillis(std::int64_t epochMs, std::int64_t &outDays, std::int64_t &outMillis) noexcept {
            outDays = epochMs / kMillisPerDay;
            outMillis = epochMs % kMillisPerDay;
            if (outMillis < 0) {
                outMillis += kMillisPerDay;
                outDays -= 1;
            }
        }

        inline int SafeMod(int value, int modulus) noexcept {
            int result = value % modulus;
            if (result < 0) {
                result += modulus;
            }
            return result;
        }

        inline bool IsDigit(char c) noexcept {
            return c >= '0' && c <= '9';
        }

        inline int ParseInt(std::string_view text, std::size_t offset, std::size_t length) noexcept {
            int value = 0;
            for (std::size_t i = 0; i < length; ++i) {
                char c = text[offset + i];
                value = value * 10 + (c - '0');
            }
            return value;
        }

        inline void WriteTwoDigits(int value, char *dest) noexcept {
            dest[0] = static_cast<char>('0' + (value / 10));
            dest[1] = static_cast<char>('0' + (value % 10));
        }

        inline void WriteThreeDigits(int value, char *dest) noexcept {
            dest[0] = static_cast<char>('0' + (value / 100));
            dest[1] = static_cast<char>('0' + ((value / 10) % 10));
            dest[2] = static_cast<char>('0' + (value % 10));
        }
    }

    DateModule::Metrics::Metrics() noexcept
        : allocations(0),
          releases(0),
          canonicalHits(0),
          canonicalMisses(0),
          epochConstructs(0),
          componentConstructs(0),
          nowCalls(0),
          componentConversions(0),
          isoFormats(0),
          isoParses(0),
          arithmeticOps(0),
          lastFrameTouched(0),
          hotDates(0),
          gpuOptimized(false) {
    }

    DateModule::DateModule()
        : m_Runtime(nullptr),
          m_Subsystems(nullptr),
          m_Config{},
          m_GpuEnabled(false),
          m_Initialized(false),
          m_CurrentFrame(0),
          m_Slots{},
          m_FreeSlots{},
          m_CanonicalEpoch(0),
          m_Metrics() {
    }

    std::string_view DateModule::Name() const noexcept {
        return kName;
    }

    std::string_view DateModule::Summary() const noexcept {
        return kSummary;
    }

    std::string_view DateModule::SpecificationReference() const noexcept {
        return kReference;
    }

    void DateModule::Initialize(const ModuleInitContext &context) {
        m_Runtime = &context.runtime;
        m_Subsystems = &context.subsystems;
        m_Config = context.config;
        m_GpuEnabled = context.config.enableGpuAcceleration;
        Reset();
        m_Initialized = true;
    }

    void DateModule::Tick(const TickInfo &info, const ModuleTickContext &) noexcept {
        m_CurrentFrame = info.frameIndex;
        RecomputeHotMetrics();
    }

    void DateModule::OptimizeGpu(const ModuleGpuContext &context) noexcept {
        m_GpuEnabled = context.enableAcceleration;
        m_Metrics.gpuOptimized = m_GpuEnabled;
    }

    void DateModule::Reconfigure(const RuntimeConfig &config) {
        m_Config = config;
        m_GpuEnabled = config.enableGpuAcceleration;
        m_Metrics.gpuOptimized = m_GpuEnabled;
    }

    StatusCode DateModule::CreateFromEpochMilliseconds(std::string_view label, std::int64_t epochMs, Handle &outHandle) {
        return CreateInternal(label, epochMs, false, outHandle);
    }

    StatusCode DateModule::CreateFromComponents(std::string_view label,
                                                int year,
                                                int month,
                                                int day,
                                                int hour,
                                                int minute,
                                                int second,
                                                int millisecond,
                                                Handle &outHandle) {
        if (!ValidateComponents(year, month, day, hour, minute, second, millisecond)) {
            outHandle = 0;
            return StatusCode::InvalidArgument;
        }
        auto epoch = CivilToEpochMilliseconds(year, month, day, hour, minute, second, millisecond);
        auto status = CreateInternal(label, epoch, false, outHandle);
        if (status == StatusCode::Ok) {
            m_Metrics.componentConstructs += 1;
        }
        return status;
    }

    StatusCode DateModule::Now(std::string_view label, Handle &outHandle) {
        auto now = std::chrono::time_point_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now());
        auto epochMs = static_cast<std::int64_t>(now.time_since_epoch().count());
        auto status = CreateInternal(label, epochMs, false, outHandle);
        if (status == StatusCode::Ok) {
            m_Metrics.nowCalls += 1;
        }
        return status;
    }

    StatusCode DateModule::Clone(Handle handle, std::string_view label, Handle &outHandle) {
        const auto *entry = Find(handle);
        if (!entry) {
            outHandle = 0;
            return StatusCode::NotFound;
        }
        return CreateInternal(label, entry->epochMilliseconds, false, outHandle);
    }

    StatusCode DateModule::Destroy(Handle handle) {
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

    StatusCode DateModule::SetEpochMilliseconds(Handle handle, std::int64_t epochMs) noexcept {
        auto *entry = FindMutable(handle);
        if (!entry) {
            return StatusCode::NotFound;
        }
        if (entry->pinned) {
            return StatusCode::InvalidArgument;
        }
        entry->epochMilliseconds = epochMs;
        entry->componentsDirty = true;
        Touch(*entry);
        return StatusCode::Ok;
    }

    StatusCode DateModule::AddMilliseconds(Handle handle, std::int64_t delta) noexcept {
        if (delta == 0) {
            return StatusCode::Ok;
        }
        auto *entry = FindMutable(handle);
        if (!entry) {
            return StatusCode::NotFound;
        }
        if (entry->pinned) {
            return StatusCode::InvalidArgument;
        }
        if ((delta > 0 && entry->epochMilliseconds > (std::numeric_limits<std::int64_t>::max() - delta)) ||
            (delta < 0 && entry->epochMilliseconds < (std::numeric_limits<std::int64_t>::min() - delta))) {
            return StatusCode::CapacityExceeded;
        }
        entry->epochMilliseconds += delta;
        entry->componentsDirty = true;
        Touch(*entry);
        m_Metrics.arithmeticOps += 1;
        return StatusCode::Ok;
    }

    StatusCode DateModule::AddDays(Handle handle, std::int32_t days) noexcept {
        if (days == 0) {
            return StatusCode::Ok;
        }
        auto delta = static_cast<std::int64_t>(days) * kMillisPerDay;
        if (static_cast<std::int64_t>(days) != 0 && delta / days != kMillisPerDay) {
            return StatusCode::CapacityExceeded;
        }
        return AddMilliseconds(handle, delta);
    }

    StatusCode DateModule::ToComponents(Handle handle, Components &outComponents) const {
        const auto *entry = Find(handle);
        if (!entry) {
            return StatusCode::NotFound;
        }
        auto *mutableEntry = const_cast<Entry *>(entry);
        EnsureComponents(*mutableEntry);
        outComponents = mutableEntry->components;
        m_Metrics.componentConversions += 1;
        return StatusCode::Ok;
    }

    StatusCode DateModule::FormatIso8601(Handle handle, std::string &outText) const {
        const auto *entry = Find(handle);
        if (!entry) {
            outText.clear();
            return StatusCode::NotFound;
        }
        auto *mutableEntry = const_cast<Entry *>(entry);
        EnsureComponents(*mutableEntry);
        const auto &components = mutableEntry->components;
        std::array<char, 24> buffer{};
        int year = components.year;
        buffer[0] = static_cast<char>('0' + ((year / 1000) % 10));
        buffer[1] = static_cast<char>('0' + ((year / 100) % 10));
        buffer[2] = static_cast<char>('0' + ((year / 10) % 10));
        buffer[3] = static_cast<char>('0' + (year % 10));
        buffer[4] = '-';
        WriteTwoDigits(components.month, buffer.data() + 5);
        buffer[7] = '-';
        WriteTwoDigits(components.day, buffer.data() + 8);
        buffer[10] = 'T';
        WriteTwoDigits(components.hour, buffer.data() + 11);
        buffer[13] = ':';
        WriteTwoDigits(components.minute, buffer.data() + 14);
        buffer[16] = ':';
        WriteTwoDigits(components.second, buffer.data() + 17);
        buffer[19] = '.';
        WriteThreeDigits(components.millisecond, buffer.data() + 20);
        buffer[23] = 'Z';
        outText.assign(buffer.data(), 24);
        m_Metrics.isoFormats += 1;
        return StatusCode::Ok;
    }

    StatusCode DateModule::ParseIso8601(std::string_view text, std::string_view label, Handle &outHandle) {
        Components components{};
        if (!ParseIso8601Internal(text, components)) {
            outHandle = 0;
            return StatusCode::InvalidArgument;
        }
        auto status = CreateFromComponents(label,
                                           components.year,
                                           components.month,
                                           components.day,
                                           components.hour,
                                           components.minute,
                                           components.second,
                                           components.millisecond,
                                           outHandle);
        if (status == StatusCode::Ok) {
            m_Metrics.isoParses += 1;
        }
        return status;
    }

    StatusCode DateModule::DifferenceMilliseconds(Handle left, Handle right, std::int64_t &outDelta) const noexcept {
        const auto *lhs = Find(left);
        const auto *rhs = Find(right);
        if (!lhs || !rhs) {
            outDelta = 0;
            return StatusCode::NotFound;
        }
        outDelta = lhs->epochMilliseconds - rhs->epochMilliseconds;
        m_Metrics.arithmeticOps += 1;
        return StatusCode::Ok;
    }

    bool DateModule::Has(Handle handle) const noexcept {
        return Find(handle) != nullptr;
    }

    std::int64_t DateModule::EpochMilliseconds(Handle handle, std::int64_t fallback) const noexcept {
        const auto *entry = Find(handle);
        return entry ? entry->epochMilliseconds : fallback;
    }

    DateModule::Handle DateModule::CanonicalEpoch() noexcept {
        m_Metrics.canonicalHits += 1;
        return m_CanonicalEpoch;
    }

    const DateModule::Metrics &DateModule::GetMetrics() const noexcept {
        return m_Metrics;
    }

    bool DateModule::GpuEnabled() const noexcept {
        return m_GpuEnabled;
    }

    DateModule::Entry *DateModule::FindMutable(Handle handle) noexcept {
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

    const DateModule::Entry *DateModule::Find(Handle handle) const noexcept {
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

    std::uint32_t DateModule::DecodeSlot(Handle handle) noexcept {
        return static_cast<std::uint32_t>(handle & 0xffffffffull);
    }

    std::uint32_t DateModule::DecodeGeneration(Handle handle) noexcept {
        return static_cast<std::uint32_t>((handle >> 32) & 0xffffffffull);
    }

    DateModule::Handle DateModule::EncodeHandle(std::uint32_t slot, std::uint32_t generation) noexcept {
        return (static_cast<Handle>(generation) << 32) | static_cast<Handle>(slot);
    }

    StatusCode DateModule::CreateInternal(std::string_view label, std::int64_t epochMs, bool pinned, Handle &outHandle) {
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
        slot.entry.epochMilliseconds = epochMs;
        slot.entry.componentsDirty = true;
        slot.entry.version = 0;
        slot.entry.lastTouchFrame = m_CurrentFrame;
        slot.entry.label.assign(label);
        slot.entry.hot = true;
        slot.entry.pinned = pinned;
        outHandle = slot.entry.handle;
        Touch(slot.entry);
        if (!pinned) {
            m_Metrics.allocations += 1;
        }
        m_Metrics.epochConstructs += 1;
        return StatusCode::Ok;
    }

    void DateModule::Reset() {
        m_Slots.clear();
        m_FreeSlots.clear();
        m_CurrentFrame = 0;
        m_Metrics = Metrics();
        m_Metrics.gpuOptimized = m_GpuEnabled;
        Handle epoch = 0;
        CreateInternal("date.epoch", 0, true, epoch);
        m_CanonicalEpoch = epoch;
    }

    void DateModule::Touch(Entry &entry) noexcept {
        entry.version += 1;
        entry.lastTouchFrame = m_CurrentFrame;
        entry.hot = true;
        m_Metrics.lastFrameTouched = m_CurrentFrame;
    }

    void DateModule::RecomputeHotMetrics() noexcept {
        std::uint64_t hot = 0;
        for (auto &slot : m_Slots) {
            if (!slot.inUse) {
                continue;
            }
            auto &entry = slot.entry;
            entry.hot = (m_CurrentFrame - entry.lastTouchFrame) <= kHotFrameWindow;
            if (entry.hot) {
                ++hot;
            }
        }
        m_Metrics.hotDates = hot;
        m_Metrics.lastFrameTouched = m_CurrentFrame;
    }

    void DateModule::EnsureComponents(Entry &entry) const {
        if (!entry.componentsDirty) {
            return;
        }
        EpochMillisecondsToCivil(entry.epochMilliseconds, entry.components);
        entry.componentsDirty = false;
    }

    bool DateModule::ValidateComponents(int year,
                                         int month,
                                         int day,
                                         int hour,
                                         int minute,
                                         int second,
                                         int millisecond) noexcept {
        if (month < 1 || month > 12) {
            return false;
        }
        if (hour < 0 || hour >= 24 || minute < 0 || minute >= 60 || second < 0 || second >= 60) {
            return false;
        }
        if (millisecond < 0 || millisecond >= 1000) {
            return false;
        }
        int dim = DaysInMonth(year, month);
        if (day < 1 || day > dim) {
            return false;
        }
        return true;
    }

    std::int64_t DateModule::CivilToEpochMilliseconds(int year,
                                                      int month,
                                                      int day,
                                                      int hour,
                                                      int minute,
                                                      int second,
                                                      int millisecond) noexcept {
        auto days = DaysFromCivil(year, month, day);
        std::int64_t millis = days * kMillisPerDay;
        millis += static_cast<std::int64_t>(hour) * kMillisPerHour;
        millis += static_cast<std::int64_t>(minute) * kMillisPerMinute;
        millis += static_cast<std::int64_t>(second) * kMillisPerSecond;
        millis += millisecond;
        return millis;
    }

    void DateModule::EpochMillisecondsToCivil(std::int64_t epochMs, Components &outComponents) noexcept {
        std::int64_t days = 0;
        std::int64_t millis = 0;
        SplitDaysAndMillis(epochMs, days, millis);

        int year = 0;
        unsigned month = 0;
        unsigned day = 0;
        CivilFromDays(days, year, month, day);

        int hour = static_cast<int>(millis / kMillisPerHour);
        millis %= kMillisPerHour;
        int minute = static_cast<int>(millis / kMillisPerMinute);
        millis %= kMillisPerMinute;
        int second = static_cast<int>(millis / kMillisPerSecond);
        millis %= kMillisPerSecond;
        int millisecond = static_cast<int>(millis);

        outComponents.year = year;
        outComponents.month = static_cast<int>(month);
        outComponents.day = static_cast<int>(day);
        outComponents.hour = hour;
        outComponents.minute = minute;
        outComponents.second = second;
        outComponents.millisecond = millisecond;

        int dayOfWeek = SafeMod(static_cast<int>(days) + 4, kDaysPerWeek);
        outComponents.dayOfWeek = dayOfWeek;
        const int *offsets = IsLeap(year) ? kMonthOffsetsLeap : kMonthOffsets;
        outComponents.dayOfYear = offsets[outComponents.month - 1] + outComponents.day;
    }

    bool DateModule::ParseIso8601Internal(std::string_view text, Components &outComponents) noexcept {
        if (text.size() < 20) {
            return false;
        }
        // Expect YYYY-MM-DDTHH:MM:SS(.sss)?Z
        if (!(IsDigit(text[0]) && IsDigit(text[1]) && IsDigit(text[2]) && IsDigit(text[3]) &&
              text[4] == '-' && IsDigit(text[5]) && IsDigit(text[6]) && text[7] == '-' &&
              IsDigit(text[8]) && IsDigit(text[9]) && (text[10] == 'T' || text[10] == 't') &&
              IsDigit(text[11]) && IsDigit(text[12]) && text[13] == ':' && IsDigit(text[14]) &&
              IsDigit(text[15]) && text[16] == ':' && IsDigit(text[17]) && IsDigit(text[18]))) {
            return false;
        }
        int year = ParseInt(text, 0, 4);
        int month = ParseInt(text, 5, 2);
        int day = ParseInt(text, 8, 2);
        int hour = ParseInt(text, 11, 2);
        int minute = ParseInt(text, 14, 2);
        int second = ParseInt(text, 17, 2);
        int millisecond = 0;
        std::size_t index = 19;
        if (index < text.size() && text[index] == '.') {
            if (index + 3 >= text.size()) {
                return false;
            }
            if (!IsDigit(text[index + 1]) || !IsDigit(text[index + 2]) || !IsDigit(text[index + 3])) {
                return false;
            }
            millisecond = ParseInt(text, index + 1, 3);
            index += 4;
        }
        if (index >= text.size() || (text[index] != 'Z' && text[index] != 'z')) {
            return false;
        }
        if (!ValidateComponents(year, month, day, hour, minute, second, millisecond)) {
            return false;
        }
        outComponents.year = year;
        outComponents.month = month;
        outComponents.day = day;
        outComponents.hour = hour;
        outComponents.minute = minute;
        outComponents.second = second;
        outComponents.millisecond = millisecond;
        return true;
    }
}

