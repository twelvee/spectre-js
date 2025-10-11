#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "spectre/config.h"
#include "spectre/es2025/module.h"
#include "spectre/status.h"

namespace spectre::es2025 {
    class IntlModule final : public Module {
    public:
        using LocaleHandle = std::uint32_t;
        static constexpr LocaleHandle kInvalidLocale = 0;

        enum class NumberStyle : std::uint8_t {
            Decimal,
            Currency,
            Percent
        };

        enum class SignDisplay : std::uint8_t {
            Auto,
            Never,
            Always,
            ExceptZero
        };

        enum class CurrencyDisplay : std::uint8_t {
            Code,
            Symbol
        };

        enum class Notation : std::uint8_t {
            Standard,
            Compact
        };

        enum class DateStyle : std::uint8_t {
            None,
            Short,
            Medium,
            Long
        };

        enum class TimeStyle : std::uint8_t {
            None,
            Short,
            Medium,
            Long
        };

        enum class TimeZone : std::uint8_t {
            Local,
            Utc
        };

        enum class ListType : std::uint8_t {
            Conjunction,
            Disjunction,
            Unit
        };

        enum class ListStyle : std::uint8_t {
            Long,
            Short,
            Narrow
        };

        static constexpr std::size_t kListPatternVariants = 9;

        struct NumberFormatOptions {
            NumberStyle style;
            SignDisplay signDisplay;
            CurrencyDisplay currencyDisplay;
            Notation notation;
            bool useGrouping;
            std::uint8_t minimumFractionDigits;
            std::uint8_t maximumFractionDigits;
            std::string currency;

            NumberFormatOptions() noexcept;
        };

        struct DateTimeFormatOptions {
            DateStyle dateStyle;
            TimeStyle timeStyle;
            TimeZone timeZone;
            bool fractionalSeconds;

            DateTimeFormatOptions() noexcept;
        };

        struct ListFormatOptions {
            ListType type;
            ListStyle style;

            ListFormatOptions() noexcept;
        };

        struct LocaleBlueprint {
            struct ListPatternBlueprint {
                std::string_view pair;
                std::string_view start;
                std::string_view middle;
                std::string_view end;
            };
            std::string_view identifier;
            char decimalSeparator;
            char groupSeparator;
            std::array<std::uint8_t, 3> grouping;
            std::string_view currencyCode;
            std::string_view currencySymbol;
            std::string_view percentSymbol;
            std::string_view nanSymbol;
            std::string_view infinitySymbol;
            std::string_view positivePattern;
            std::string_view negativePattern;
            std::string_view currencyPositivePattern;
            std::string_view currencyNegativePattern;
            std::string_view percentPositivePattern;
            std::string_view percentNegativePattern;
            std::string_view datePatternShort;
            std::string_view datePatternMedium;
            std::string_view datePatternLong;
            std::string_view timePatternShort;
            std::string_view timePatternMedium;
            std::string_view timePatternLong;
            std::string_view dateTimeSeparator;
            std::string_view amDesignator;
            std::string_view pmDesignator;
            std::array<std::string_view, 12> monthNamesShort;
            std::array<std::string_view, 12> monthNamesLong;
            std::array<std::string_view, 7> weekdayNamesShort;
            std::array<std::string_view, 7> weekdayNamesLong;
            std::array<ListPatternBlueprint, kListPatternVariants> listPatterns;

            LocaleBlueprint() noexcept;
        };

        struct FormatResult {
            StatusCode status;
            std::string value;
            std::string diagnostics;
            LocaleHandle locale;
            std::uint64_t localeVersion;
            std::uint64_t formatterVersion;

            FormatResult() noexcept;
        };

        struct LocaleSnapshot {
            LocaleHandle handle;
            std::string identifier;
            std::string currencyCode;
            std::string currencySymbol;
            char decimalSeparator;
            char groupSeparator;
            std::array<std::uint8_t, 3> grouping;
            std::uint64_t version;
            bool active;

            LocaleSnapshot() noexcept;
        };

        struct Metrics {
            std::uint64_t localesRegistered;
            std::uint64_t localesUpdated;
            std::uint64_t numberFormatRequests;
            std::uint64_t numberFormatterHits;
            std::uint64_t dateFormatRequests;
            std::uint64_t dateFormatterHits;
            std::uint64_t listFormatRequests;
            std::uint64_t listFormatterHits;
            std::uint64_t cacheEvictions;
            std::uint64_t currentFrame;
            std::size_t numberCacheSize;
            std::size_t dateCacheSize;
            std::size_t listCacheSize;
            bool gpuOptimized;

            Metrics() noexcept;
        };

        IntlModule();

        std::string_view Name() const noexcept override;
        std::string_view Summary() const noexcept override;
        std::string_view SpecificationReference() const noexcept override;

        void Initialize(const ModuleInitContext &context) override;
        void Tick(const TickInfo &info, const ModuleTickContext &context) noexcept override;
        void OptimizeGpu(const ModuleGpuContext &context) noexcept override;
        void Reconfigure(const RuntimeConfig &config) override;

        StatusCode RegisterLocale(const LocaleBlueprint &blueprint, LocaleHandle &outHandle);
        StatusCode EnsureLocale(std::string_view identifier, LocaleHandle &outHandle);
        bool HasLocale(std::string_view identifier) const noexcept;

        FormatResult FormatNumber(LocaleHandle locale,
                                  double value,
                                  const NumberFormatOptions &options);
        FormatResult FormatNumber(std::string_view identifier,
                                  double value,
                                  const NumberFormatOptions &options);

        FormatResult FormatDateTime(LocaleHandle locale,
                                    std::chrono::system_clock::time_point value,
                                    const DateTimeFormatOptions &options);
        FormatResult FormatDateTime(std::string_view identifier,
                                    std::chrono::system_clock::time_point value,
                                    const DateTimeFormatOptions &options);

        FormatResult FormatList(LocaleHandle locale,
                                std::span<const std::string_view> values,
                                const ListFormatOptions &options);
        FormatResult FormatList(LocaleHandle locale,
                                std::initializer_list<std::string_view> values,
                                const ListFormatOptions &options);
        FormatResult FormatList(std::string_view identifier,
                                std::span<const std::string_view> values,
                                const ListFormatOptions &options);
        FormatResult FormatList(std::string_view identifier,
                                std::initializer_list<std::string_view> values,
                                const ListFormatOptions &options);

        LocaleSnapshot Snapshot(LocaleHandle locale) const;
        void Clear(bool releaseCapacity = false);
        const Metrics &GetMetrics() const noexcept;

        std::string DefaultLocale() const noexcept;
        void SetDefaultLocale(std::string_view identifier);

    private:
        struct LocaleData {
            char decimalSeparator;
            char groupSeparator;
            std::array<std::uint8_t, 3> grouping;
            std::string currencyCode;
            std::string currencySymbol;
            std::string percentSymbol;
            std::string nanSymbol;
            std::string infinitySymbol;
            std::string positivePattern;
            std::string negativePattern;
            std::string currencyPositivePattern;
            std::string currencyNegativePattern;
            std::string percentPositivePattern;
            std::string percentNegativePattern;
            std::array<std::string, 12> monthNamesShort;
            std::array<std::string, 12> monthNamesLong;
            std::array<std::string, 7> weekdayNamesShort;
            std::array<std::string, 7> weekdayNamesLong;
            std::array<std::string, 3> datePatterns;
            std::array<std::string, 3> timePatterns;
            std::string dateTimeSeparator;
            std::string amDesignator;
            std::string pmDesignator;
            struct ListPattern {
                std::string pair;
                std::string start;
                std::string middle;
                std::string end;
            };
            std::array<ListPattern, kListPatternVariants> listPatterns;

            LocaleData() noexcept;
        };

        friend std::string FormatPattern(const LocaleData &, const std::tm &, std::string_view, bool &, int);

        struct LocaleRecord {
            LocaleHandle handle;
            std::string identifier;
            LocaleData data;
            std::uint64_t version;
            std::uint64_t lastUsedFrame;
            bool active;

            LocaleRecord() noexcept;
        };

        struct LocaleSlot {
            LocaleRecord record;
            std::uint16_t generation;
            bool inUse;

            LocaleSlot() noexcept;
        };

        struct TransparentHash {
            using is_transparent = void;

            std::size_t operator()(const std::string &value) const noexcept;
            std::size_t operator()(std::string_view value) const noexcept;
            std::size_t operator()(const char *value) const noexcept;
        };

        struct TransparentEqual {
            using is_transparent = void;

            bool operator()(std::string_view lhs, std::string_view rhs) const noexcept;
        };

        struct NumberFormatterDescriptor {
            NumberStyle style;
            SignDisplay signDisplay;
            CurrencyDisplay currencyDisplay;
            Notation notation;
            bool useGrouping;
            std::uint8_t minFractionDigits;
            std::uint8_t maxFractionDigits;
            std::string currencyCode;
            std::string currencySymbol;
            std::string percentSymbol;

            NumberFormatterDescriptor() noexcept;
            bool Equals(const NumberFormatterDescriptor &other) const noexcept;
        };

        struct NumberFormatterCacheEntry {
            NumberFormatterDescriptor descriptor;
            LocaleHandle locale;
            std::uint16_t generation;
            std::uint64_t signature;
            std::uint64_t hits;
            std::uint64_t lastUsedFrame;

            NumberFormatterCacheEntry() noexcept;
        };

        struct DateFormatterDescriptor {
            DateStyle dateStyle;
            TimeStyle timeStyle;
            TimeZone timeZone;
            bool fractionalSeconds;

            DateFormatterDescriptor() noexcept;
            bool Equals(const DateFormatterDescriptor &other) const noexcept;
        };

        struct DateFormatterCacheEntry {
            DateFormatterDescriptor descriptor;
            LocaleHandle locale;
            std::uint16_t generation;
            std::uint64_t signature;
            std::uint64_t hits;
            std::uint64_t lastUsedFrame;

            DateFormatterCacheEntry() noexcept;
        };

        struct ListFormatterDescriptor {
            ListType type;
            ListStyle style;

            ListFormatterDescriptor() noexcept;
            bool Equals(const ListFormatterDescriptor &other) const noexcept;
        };

        struct ListFormatterCacheEntry {
            ListFormatterDescriptor descriptor;
            LocaleHandle locale;
            std::uint16_t generation;
            std::uint64_t signature;
            std::uint64_t hits;
            std::uint64_t lastUsedFrame;

            ListFormatterCacheEntry() noexcept;
        };

        static constexpr std::size_t kHandleIndexBits = 16;
        static constexpr std::size_t kHandleIndexMask = (1u << kHandleIndexBits) - 1;

        static LocaleHandle MakeHandle(std::size_t index, std::uint16_t generation) noexcept;
        static std::size_t ExtractIndex(LocaleHandle handle) noexcept;
        static std::uint16_t ExtractGeneration(LocaleHandle handle) noexcept;
        static std::uint16_t NextGeneration(std::uint16_t generation) noexcept;

        LocaleSlot *ResolveSlot(LocaleHandle handle) noexcept;
        const LocaleSlot *ResolveSlot(LocaleHandle handle) const noexcept;

        std::size_t AcquireSlot();
        void ReleaseSlot(std::size_t index) noexcept;

        void InstallBuiltinLocales();
        void ApplyBlueprint(LocaleRecord &record, const LocaleBlueprint &blueprint);
        void ClearLocked(bool releaseCapacity);

        void TouchLocale(LocaleRecord &record) noexcept;

        StatusCode RegisterLocaleLocked(const LocaleBlueprint &blueprint, LocaleHandle &outHandle);
        StatusCode EnsureLocaleLocked(std::string_view identifier, LocaleHandle &outHandle);

        void PurgeCachesForLocale(LocaleHandle handle);

        std::uint64_t ComputeNumberSignature(LocaleHandle locale,
                                             const NumberFormatterDescriptor &descriptor) const noexcept;
        std::uint64_t ComputeDateSignature(LocaleHandle locale,
                                           const DateFormatterDescriptor &descriptor) const noexcept;
        std::uint64_t ComputeListSignature(LocaleHandle locale,
                                           const ListFormatterDescriptor &descriptor) const noexcept;

        std::string FormatNumberInternal(const LocaleRecord &record,
                                         const NumberFormatterDescriptor &descriptor,
                                         double value) const;
        std::string FormatDateTimeInternal(const LocaleRecord &record,
                                           const DateFormatterDescriptor &descriptor,
                                           std::chrono::system_clock::time_point value) const;
        std::string FormatListInternal(const LocaleRecord &record,
                                       const ListFormatterDescriptor &descriptor,
                                       std::span<const std::string_view> values) const;

        void EvictStaleFormatters();

        SpectreRuntime *m_Runtime;
        detail::SubsystemSuite *m_Subsystems;
        RuntimeConfig m_Config;
        bool m_GpuEnabled;
        bool m_Initialized;
        std::uint64_t m_CurrentFrame;
        double m_TotalSeconds;

        std::string m_DefaultLocale;
        LocaleHandle m_DefaultLocaleHandle;

        std::vector<LocaleSlot> m_Slots;
        std::vector<std::uint32_t> m_FreeList;
        std::unordered_map<std::string, LocaleHandle, TransparentHash, TransparentEqual> m_LocaleLookup;

        std::unordered_map<std::uint64_t, NumberFormatterCacheEntry> m_NumberCache;
        std::unordered_map<std::uint64_t, DateFormatterCacheEntry> m_DateCache;
        std::unordered_map<std::uint64_t, ListFormatterCacheEntry> m_ListCache;

        Metrics m_Metrics;

        mutable std::mutex m_Mutex;
    };
}

