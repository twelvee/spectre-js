#include "spectre/es2025/modules/intl_module.h"

#include <iostream>
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <limits>

#include "spectre/runtime.h"

namespace spectre::es2025 {
    namespace {
        constexpr std::string_view kName = "Intl";
        constexpr std::string_view kSummary =
                "Locale-aware number, date, and list formatting with aggressive caching.";
        constexpr std::string_view kReference = "ECMA-402";
        constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
        constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
        constexpr std::size_t kMaxFractionDigits = 9;
        constexpr std::size_t kNumberCacheLimit = 512;
        constexpr std::size_t kDateCacheLimit = 256;
        constexpr std::size_t kListCacheLimit = 256;
        constexpr std::uint64_t kCacheHorizonFrames = 720;
        constexpr std::array<std::uint64_t, 10> kPow10{
            1ULL,
            10ULL,
            100ULL,
            1000ULL,
            10000ULL,
            100000ULL,
            1000000ULL,
            10000000ULL,
            100000000ULL,
            1000000000ULL
        };


        std::uint64_t HashString(std::string_view value) noexcept {
            std::uint64_t hash = kFnvOffset;
            for (unsigned char ch: value) {
                hash ^= static_cast<std::uint64_t>(ch);
                hash *= kFnvPrime;
            }
            return hash;
        }

        bool IsZero(double value) noexcept {
            return std::fabs(value) < 1e-12;
        }

        void ApplyGrouping(std::string &digits,
                           char separator,
                           const std::array<std::uint8_t, 3> &grouping) {
            if (separator == '\0' || digits.size() < 4) {
                return;
            }
            std::uint8_t first = grouping[0];
            if (first == 0) {
                return;
            }
            std::string grouped;
            grouped.reserve(digits.size() + digits.size() / static_cast<std::size_t>(first) + 4);
            std::size_t i = digits.size();
            std::size_t groupIndex = 0;
            std::uint8_t currentGroup = grouping[groupIndex];
            std::size_t count = 0;
            while (i > 0) {
                char ch = digits[--i];
                grouped.push_back(ch);
                ++count;
                if (i == 0) {
                    break;
                }
                if (count == currentGroup) {
                    grouped.push_back(separator);
                    count = 0;
                    if (groupIndex + 1 < grouping.size() && grouping[groupIndex + 1] != 0) {
                        ++groupIndex;
                        currentGroup = grouping[groupIndex];
                    }
                }
            }
            std::reverse(grouped.begin(), grouped.end());
            digits.swap(grouped);
        }

        void AppendPattern(const std::string &pattern,
                           std::string_view value,
                           std::string_view primary,
                           std::string_view secondary,
                           std::string &out) {
            for (std::size_t i = 0; i < pattern.size();) {
                if (pattern[i] == '{') {
                    auto close = pattern.find('}', i);
                    if (close == std::string::npos) {
                        out.push_back(pattern[i]);
                        ++i;
                        continue;
                    }
                    auto token = pattern.substr(i + 1, close - i - 1);
                    if (token == "value") {
                        out.append(value);
                    } else if (token == "symbol") {
                        out.append(primary);
                    } else if (token == "code") {
                        out.append(secondary.empty() ? primary : secondary);
                    } else if (token == "percent") {
                        out.append(primary);
                    } else {
                        out.append(pattern.substr(i, close - i + 1));
                    }
                    i = close + 1;
                } else {
                    out.push_back(pattern[i]);
                    ++i;
                }
            }
        }


        void ApplyListPattern(const std::string &pattern,
                               std::string_view first,
                               std::string_view second,
                               std::string &out) {
            for (std::size_t i = 0; i < pattern.size();) {
                if (pattern[i] == '{' && i + 2 < pattern.size() && pattern[i + 2] == '}') {
                    char index = pattern[i + 1];
                    if (index == '0') {
                        out.append(first);
                        i += 3;
                        continue;
                    }
                    if (index == '1') {
                        out.append(second);
                        i += 3;
                        continue;
                    }
                }
                out.push_back(pattern[i]);
                ++i;
            }
        }

        constexpr std::size_t EncodeListPatternIndex(IntlModule::ListType type,
                                                     IntlModule::ListStyle style) noexcept {
            return static_cast<std::size_t>(type) * 3 + static_cast<std::size_t>(style);
        }


        void AppendZeroPadded(int value, std::size_t width, std::string &out) {
            char buffer[32];
            auto [ptr, ec] = std::to_chars(buffer, buffer + sizeof(buffer), value);
            if (ec != std::errc()) {
                std::snprintf(buffer, sizeof(buffer), "%d", value);
                ptr = buffer + std::strlen(buffer);
            }
            std::size_t length = static_cast<std::size_t>(ptr - buffer);
            if (length < width) {
                out.append(width - length, '0');
            }
            out.append(buffer, length);
        }

        bool ConvertTime(std::time_t seconds, IntlModule::TimeZone zone, std::tm &out) noexcept {
#if defined(_WIN32)
            if (zone == IntlModule::TimeZone::Utc) {
                return _gmtime64_s(&out, &seconds) == 0;
            }
            return _localtime64_s(&out, &seconds) == 0;
#else
            if (zone == IntlModule::TimeZone::Utc) {
                return gmtime_r(&seconds, &out) != nullptr;
            }
            return localtime_r(&seconds, &out) != nullptr;
#endif
        }
    } // namespace

    std::string FormatPattern(const IntlModule::LocaleData &data,
                              const std::tm &tmValue,
                              std::string_view pattern,
                              bool &fractionPending,
                              int milliseconds) {
        std::string result;
        result.reserve(pattern.size() + 32);
        bool literal = false;
        for (std::size_t i = 0; i < pattern.size();) {
            char ch = pattern[i];
            if (ch == '\'') {
                if (i + 1 < pattern.size() && pattern[i + 1] == '\'') {
                    result.push_back('\'');
                    i += 2;
                    continue;
                }
                literal = !literal;
                ++i;
                continue;
            }
            if (literal) {
                result.push_back(ch);
                ++i;
                continue;
            }
            std::size_t run = 1;
            while (i + run < pattern.size() && pattern[i + run] == ch) {
                ++run;
            }
            switch (ch) {
                case 'y': {
                    int year = tmValue.tm_year + 1900;
                    if (run >= 4) {
                        AppendZeroPadded(year, 4, result);
                    } else if (run == 3) {
                        AppendZeroPadded(year, 3, result);
                    } else if (run == 2) {
                        AppendZeroPadded(year % 100, 2, result);
                    } else {
                        AppendZeroPadded(year, 1, result);
                    }
                    break;
                }
                case 'M': {
                    int month = tmValue.tm_mon;
                    if (month < 0) {
                        month = 0;
                    }
                    month %= 12;
                    if (run >= 4) {
                        result.append(data.monthNamesLong[static_cast<std::size_t>(month)]);
                    } else if (run == 3) {
                        result.append(data.monthNamesShort[static_cast<std::size_t>(month)]);
                    } else {
                        int numeric = month + 1;
                        if (run == 2) {
                            AppendZeroPadded(numeric, 2, result);
                        } else {
                            AppendZeroPadded(numeric, 1, result);
                        }
                    }
                    break;
                }
                case 'd': {
                    int day = tmValue.tm_mday;
                    if (run >= 2) {
                        AppendZeroPadded(day, 2, result);
                    } else {
                        AppendZeroPadded(day, 1, result);
                    }
                    break;
                }
                case 'H': {
                    int hour = tmValue.tm_hour;
                    if (run >= 2) {
                        AppendZeroPadded(hour, 2, result);
                    } else {
                        AppendZeroPadded(hour, 1, result);
                    }
                    break;
                }
                case 'h': {
                    int hour = tmValue.tm_hour % 12;
                    if (hour == 0) {
                        hour = 12;
                    }
                    if (run >= 2) {
                        AppendZeroPadded(hour, 2, result);
                    } else {
                        AppendZeroPadded(hour, 1, result);
                    }
                    break;
                }
                case 'm': {
                    int minute = tmValue.tm_min;
                    if (run >= 2) {
                        AppendZeroPadded(minute, 2, result);
                    } else {
                        AppendZeroPadded(minute, 1, result);
                    }
                    break;
                }
                case 's': {
                    int second = tmValue.tm_sec;
                    if (run >= 2) {
                        AppendZeroPadded(second, 2, result);
                    } else {
                        AppendZeroPadded(second, 1, result);
                    }
                    if (fractionPending) {
                        result.push_back('.');
                        if (milliseconds < 0) {
                            AppendZeroPadded(0, 3, result);
                        } else {
                            AppendZeroPadded(milliseconds, 3, result);
                        }
                        fractionPending = false;
                    }
                    break;
                }
                case 'E': {
                    int weekday = tmValue.tm_wday;
                    if (weekday < 0) {
                        weekday = 0;
                    }
                    weekday %= 7;
                    if (run >= 4) {
                        result.append(data.weekdayNamesLong[static_cast<std::size_t>(weekday)]);
                    } else {
                        result.append(data.weekdayNamesShort[static_cast<std::size_t>(weekday)]);
                    }
                    break;
                }
                case 'a': {
                    if (tmValue.tm_hour >= 12) {
                        result.append(data.pmDesignator);
                    } else {
                        result.append(data.amDesignator);
                    }
                    break;
                }
                default: {
                    result.append(pattern.substr(i, run));
                    break;
                }
            }
            i += run;
        }
        return result;
    }



    IntlModule::NumberFormatOptions::NumberFormatOptions() noexcept
        : style(NumberStyle::Decimal),
          signDisplay(SignDisplay::Auto),
          currencyDisplay(CurrencyDisplay::Symbol),
          notation(Notation::Standard),
          useGrouping(true),
          minimumFractionDigits(0),
          maximumFractionDigits(3),
          currency() {
    }

    IntlModule::DateTimeFormatOptions::DateTimeFormatOptions() noexcept
        : dateStyle(DateStyle::Short),
          timeStyle(TimeStyle::Short),
          timeZone(TimeZone::Local),
          fractionalSeconds(false) {
    }

    IntlModule::ListFormatOptions::ListFormatOptions() noexcept
        : type(ListType::Conjunction),
          style(ListStyle::Long) {
    }

    IntlModule::LocaleBlueprint::LocaleBlueprint() noexcept
        : identifier("en-US"),
          decimalSeparator('.'),
          groupSeparator(','),
          grouping{3, 0, 0},
          currencyCode("USD"),
          currencySymbol("$"),
          percentSymbol("%"),
          nanSymbol("NaN"),
          infinitySymbol("Infinity"),
          positivePattern("{value}"),
          negativePattern("-{value}"),
          currencyPositivePattern("{symbol}{value}"),
          currencyNegativePattern("-{symbol}{value}"),
          percentPositivePattern("{value}%"),
          percentNegativePattern("-{value}%"),
          datePatternShort("M/d/yy"),
          datePatternMedium("MMM d, yyyy"),
          datePatternLong("MMMM d, yyyy"),
          timePatternShort("h:mm a"),
          timePatternMedium("h:mm:ss a"),
          timePatternLong("h:mm:ss a"),
          dateTimeSeparator(" "),
          amDesignator("AM"),
          pmDesignator("PM"),
          monthNamesShort{"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"},
          monthNamesLong{"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"},
          weekdayNamesShort{"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"},
          weekdayNamesLong{"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"},
          listPatterns{{
              ListPatternBlueprint{"{0} and {1}", "{0}, {1}", "{0}, {1}", "{0}, and {1}"},
              ListPatternBlueprint{"{0} & {1}", "{0}, {1}", "{0}, {1}", "{0}, & {1}"},
              ListPatternBlueprint{"{0} & {1}", "{0}, {1}", "{0}, {1}", "{0}, & {1}"},
              ListPatternBlueprint{"{0} or {1}", "{0}, {1}", "{0}, {1}", "{0}, or {1}"},
              ListPatternBlueprint{"{0} or {1}", "{0}, {1}", "{0}, {1}", "{0}, or {1}"},
              ListPatternBlueprint{"{0} or {1}", "{0}, {1}", "{0}, {1}", "{0}, or {1}"},
              ListPatternBlueprint{"{0}, {1}", "{0}, {1}", "{0}, {1}", "{0}, {1}"},
              ListPatternBlueprint{"{0}, {1}", "{0}, {1}", "{0}, {1}", "{0}, {1}"},
              ListPatternBlueprint{"{0} {1}", "{0} {1}", "{0} {1}", "{0} {1}"}
          }} {
    }

    IntlModule::FormatResult::FormatResult() noexcept
        : status(StatusCode::NotFound),
          value(),
          diagnostics(),
          locale(kInvalidLocale),
          localeVersion(0),
          formatterVersion(0) {
    }

    IntlModule::LocaleSnapshot::LocaleSnapshot() noexcept
        : handle(kInvalidLocale),
          identifier(),
          currencyCode(),
          currencySymbol(),
          decimalSeparator('.'),
          groupSeparator(','),
          grouping{3, 0, 0},
          version(0),
          active(false) {
    }

    IntlModule::Metrics::Metrics() noexcept
        : localesRegistered(0),
          localesUpdated(0),
          numberFormatRequests(0),
          numberFormatterHits(0),
          dateFormatRequests(0),
          dateFormatterHits(0),
          listFormatRequests(0),
          listFormatterHits(0),
          cacheEvictions(0),
          currentFrame(0),
          numberCacheSize(0),
          dateCacheSize(0),
          listCacheSize(0),
          gpuOptimized(false) {
    }

    IntlModule::LocaleData::LocaleData() noexcept
        : decimalSeparator('.'),
          groupSeparator(','),
          grouping{3, 0, 0},
          currencyCode("USD"),
          currencySymbol("$"),
          percentSymbol("%"),
          nanSymbol("NaN"),
          infinitySymbol("Infinity"),
          positivePattern("{value}"),
          negativePattern("-{value}"),
          currencyPositivePattern("{symbol}{value}"),
          currencyNegativePattern("-{symbol}{value}"),
          percentPositivePattern("{value}%"),
          percentNegativePattern("-{value}%"),
          monthNamesShort{"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"},
          monthNamesLong{"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"},
          weekdayNamesShort{"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"},
          weekdayNamesLong{"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"},
          datePatterns{"M/d/yy", "MMM d, yyyy", "MMMM d, yyyy"},
          timePatterns{"h:mm a", "h:mm:ss a", "h:mm:ss a"},
          dateTimeSeparator(" "),
          amDesignator("AM"),
          pmDesignator("PM"),
          listPatterns{{
              {"{0} and {1}", "{0}, {1}", "{0}, {1}", "{0}, and {1}"},
              {"{0} & {1}", "{0}, {1}", "{0}, {1}", "{0}, & {1}"},
              {"{0} & {1}", "{0}, {1}", "{0}, {1}", "{0}, & {1}"},
              {"{0} or {1}", "{0}, {1}", "{0}, {1}", "{0}, or {1}"},
              {"{0} or {1}", "{0}, {1}", "{0}, {1}", "{0}, or {1}"},
              {"{0} or {1}", "{0}, {1}", "{0}, {1}", "{0}, or {1}"},
              {"{0}, {1}", "{0}, {1}", "{0}, {1}", "{0}, {1}"},
              {"{0}, {1}", "{0}, {1}", "{0}, {1}", "{0}, {1}"},
              {"{0} {1}", "{0} {1}", "{0} {1}", "{0} {1}"}
          }} {
    }

    IntlModule::LocaleRecord::LocaleRecord() noexcept
        : handle(kInvalidLocale),
          identifier(),
          data(),
          version(0),
          lastUsedFrame(0),
          active(false) {
    }

    IntlModule::LocaleSlot::LocaleSlot() noexcept : record(), generation(1), inUse(false) {
    }

    std::size_t IntlModule::TransparentHash::operator()(const std::string &value) const noexcept {
        return std::hash<std::string_view>{}(value);
    }

    std::size_t IntlModule::TransparentHash::operator()(std::string_view value) const noexcept {
        return std::hash<std::string_view>{}(value);
    }

    std::size_t IntlModule::TransparentHash::operator()(const char *value) const noexcept {
        return std::hash<std::string_view>{}(value);
    }

    bool IntlModule::TransparentEqual::operator()(std::string_view lhs, std::string_view rhs) const noexcept {
        return lhs == rhs;
    }

    IntlModule::NumberFormatterDescriptor::NumberFormatterDescriptor() noexcept
        : style(NumberStyle::Decimal),
          signDisplay(SignDisplay::Auto),
          currencyDisplay(CurrencyDisplay::Symbol),
          notation(Notation::Standard),
          useGrouping(true),
          minFractionDigits(0),
          maxFractionDigits(3),
          currencyCode("USD"),
          currencySymbol("$"),
          percentSymbol("%") {
    }

    bool IntlModule::NumberFormatterDescriptor::Equals(const NumberFormatterDescriptor &other) const noexcept {
        return style == other.style
               && signDisplay == other.signDisplay
               && currencyDisplay == other.currencyDisplay
               && notation == other.notation
               && useGrouping == other.useGrouping
               && minFractionDigits == other.minFractionDigits
               && maxFractionDigits == other.maxFractionDigits
               && currencyCode == other.currencyCode
               && currencySymbol == other.currencySymbol
               && percentSymbol == other.percentSymbol;
    }

    IntlModule::NumberFormatterCacheEntry::NumberFormatterCacheEntry() noexcept
        : descriptor(),
          locale(kInvalidLocale),
          generation(0),
          signature(0),
          hits(0),
          lastUsedFrame(0) {
    }

    IntlModule::DateFormatterDescriptor::DateFormatterDescriptor() noexcept
        : dateStyle(DateStyle::Short),
          timeStyle(TimeStyle::Short),
          timeZone(TimeZone::Local),
          fractionalSeconds(false) {
    }

    bool IntlModule::DateFormatterDescriptor::Equals(const DateFormatterDescriptor &other) const noexcept {
        return dateStyle == other.dateStyle
               && timeStyle == other.timeStyle
               && timeZone == other.timeZone
               && fractionalSeconds == other.fractionalSeconds;
    }

    IntlModule::DateFormatterCacheEntry::DateFormatterCacheEntry() noexcept
        : descriptor(),
          locale(kInvalidLocale),
          generation(0),
          signature(0),
          hits(0),
          lastUsedFrame(0) {
    }

    IntlModule::ListFormatterDescriptor::ListFormatterDescriptor() noexcept
        : type(ListType::Conjunction),
          style(ListStyle::Long) {
    }

    bool IntlModule::ListFormatterDescriptor::Equals(const ListFormatterDescriptor &other) const noexcept {
        return type == other.type && style == other.style;
    }

    IntlModule::ListFormatterCacheEntry::ListFormatterCacheEntry() noexcept
        : descriptor(),
          locale(kInvalidLocale),
          generation(0),
          signature(0),
          hits(0),
          lastUsedFrame(0) {
    }

    IntlModule::IntlModule()
        : m_Runtime(nullptr),
          m_Subsystems(nullptr),
          m_Config{},
          m_GpuEnabled(false),
          m_Initialized(false),
          m_CurrentFrame(0),
          m_TotalSeconds(0.0),
          m_DefaultLocale("en-US"),
          m_DefaultLocaleHandle(kInvalidLocale),
          m_Slots(),
          m_FreeList(),
          m_LocaleLookup(),
          m_NumberCache(),
          m_DateCache(),
          m_ListCache(),
          m_Metrics(),
          m_Mutex() {
        m_Slots.reserve(8);
        m_NumberCache.reserve(64);
        m_DateCache.reserve(32);
        m_ListCache.reserve(32);
    }
    std::string_view IntlModule::Name() const noexcept {
        return kName;
    }

    std::string_view IntlModule::Summary() const noexcept {
        return kSummary;
    }

    std::string_view IntlModule::SpecificationReference() const noexcept {
        return kReference;
    }

    void IntlModule::Initialize(const ModuleInitContext &context) {
        std::cout << "[Intl] Initialize start" << std::endl;
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Runtime = &context.runtime;
        m_Subsystems = &context.subsystems;
        m_Config = context.config;
        m_GpuEnabled = context.config.enableGpuAcceleration;
        m_Initialized = true;
        m_CurrentFrame = 0;
        m_TotalSeconds = 0.0;
        m_Metrics = Metrics();
        ClearLocked(true);
        std::cout << "[Intl] installing locales" << std::endl;
        InstallBuiltinLocales();
        LocaleHandle handle = kInvalidLocale;
        std::cout << "[Intl] install complete" << std::endl;
        if (EnsureLocaleLocked(m_DefaultLocale, handle) == StatusCode::Ok) {
            m_DefaultLocaleHandle = handle;
        }
    }

    void IntlModule::Tick(const TickInfo &info, const ModuleTickContext &) noexcept {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (!m_Initialized) {
            return;
        }
        m_CurrentFrame = info.frameIndex;
        m_TotalSeconds += info.deltaSeconds;
        m_Metrics.currentFrame = m_CurrentFrame;
        EvictStaleFormatters();
    }

    void IntlModule::OptimizeGpu(const ModuleGpuContext &context) noexcept {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_GpuEnabled = context.enableAcceleration;
        m_Metrics.gpuOptimized = context.enableAcceleration;
    }

    void IntlModule::Reconfigure(const RuntimeConfig &config) {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Config = config;
        m_GpuEnabled = config.enableGpuAcceleration;
        m_Metrics.gpuOptimized = m_GpuEnabled;
    }

    StatusCode IntlModule::RegisterLocale(const LocaleBlueprint &blueprint, LocaleHandle &outHandle) {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return RegisterLocaleLocked(blueprint, outHandle);
    }

    StatusCode IntlModule::EnsureLocale(std::string_view identifier, LocaleHandle &outHandle) {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return EnsureLocaleLocked(identifier, outHandle);
    }

    bool IntlModule::HasLocale(std::string_view identifier) const noexcept {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_LocaleLookup.find(identifier) != m_LocaleLookup.end();
    }
    IntlModule::FormatResult IntlModule::FormatNumber(LocaleHandle locale,
                                                      double value,
                                                      const NumberFormatOptions &options) {
        std::lock_guard<std::mutex> lock(m_Mutex);
        FormatResult result;
        if (locale == kInvalidLocale) {
            result.status = StatusCode::InvalidArgument;
            result.diagnostics = "Invalid locale handle";
            return result;
        }
        auto *slot = ResolveSlot(locale);
        if (!slot) {
            result.status = StatusCode::NotFound;
            result.diagnostics = "Locale not found";
            return result;
        }
        auto &record = slot->record;
        TouchLocale(record);
        m_Metrics.numberFormatRequests += 1;

        NumberFormatterDescriptor descriptor;
        descriptor.style = options.style;
        descriptor.signDisplay = options.signDisplay;
        descriptor.currencyDisplay = options.currencyDisplay;
        descriptor.notation = options.notation;
        descriptor.useGrouping = options.useGrouping;
        descriptor.minFractionDigits = std::min<std::uint8_t>(options.minimumFractionDigits,
                                                              static_cast<std::uint8_t>(kMaxFractionDigits));
        descriptor.maxFractionDigits = std::min<std::uint8_t>(
            std::max(options.maximumFractionDigits, descriptor.minFractionDigits),
            static_cast<std::uint8_t>(kMaxFractionDigits));
        if (descriptor.minFractionDigits > descriptor.maxFractionDigits) {
            descriptor.minFractionDigits = descriptor.maxFractionDigits;
        }
        descriptor.currencyCode = options.currency.empty() ? record.data.currencyCode : options.currency;
        if (descriptor.currencyCode.empty()) {
            descriptor.currencyCode = record.data.currencyCode;
        }
        descriptor.currencySymbol = record.data.currencySymbol;
        if (!options.currency.empty()) {
            if (options.currencyDisplay == CurrencyDisplay::Code) {
                descriptor.currencySymbol = descriptor.currencyCode;
            } else {
                descriptor.currencySymbol = record.data.currencySymbol.empty()
                                            ? descriptor.currencyCode
                                            : record.data.currencySymbol;
            }
        } else if (descriptor.currencySymbol.empty()) {
            descriptor.currencySymbol = descriptor.currencyCode;
        }
        descriptor.percentSymbol = record.data.percentSymbol;

        auto signature = ComputeNumberSignature(locale, descriptor);
        std::uint64_t key = signature;
        NumberFormatterCacheEntry *entry = nullptr;
        for (int attempt = 0; attempt < 4; ++attempt) {
            auto found = m_NumberCache.find(key);
            if (found == m_NumberCache.end()) {
                break;
            }
            if (found->second.locale == locale
                && found->second.generation == ExtractGeneration(locale)
                && found->second.descriptor.Equals(descriptor)) {
                entry = &found->second;
                entry->hits += 1;
                entry->lastUsedFrame = m_CurrentFrame;
                m_Metrics.numberFormatterHits += 1;
                break;
            }
            key ^= (kFnvPrime + static_cast<std::uint64_t>(attempt + 1));
        }
        if (!entry) {
            auto inserted = m_NumberCache.emplace(key, NumberFormatterCacheEntry());
            entry = &inserted.first->second;
            entry->descriptor = descriptor;
            entry->locale = locale;
            entry->generation = ExtractGeneration(locale);
            entry->signature = key;
            entry->hits = 0;
            entry->lastUsedFrame = m_CurrentFrame;
            if (m_NumberCache.size() > kNumberCacheLimit) {
                EvictStaleFormatters();
            }
        }

        result.value = FormatNumberInternal(record, descriptor, value);
        result.status = StatusCode::Ok;
        result.locale = locale;
        result.localeVersion = record.version;
        result.formatterVersion = entry->signature;
        m_Metrics.numberCacheSize = m_NumberCache.size();
        return result;
    }

    IntlModule::FormatResult IntlModule::FormatNumber(std::string_view identifier,
                                                      double value,
                                                      const NumberFormatOptions &options) {
        LocaleHandle handle = kInvalidLocale;
        StatusCode status = EnsureLocale(identifier, handle);
        if (status != StatusCode::Ok) {
            FormatResult result;
            result.status = status;
            result.diagnostics = "Locale ensure failed";
            return result;
        }
        return FormatNumber(handle, value, options);
    }
    IntlModule::FormatResult IntlModule::FormatDateTime(LocaleHandle locale,
                                                        std::chrono::system_clock::time_point value,
                                                        const DateTimeFormatOptions &options) {
        std::lock_guard<std::mutex> lock(m_Mutex);
        FormatResult result;
        if (locale == kInvalidLocale) {
            result.status = StatusCode::InvalidArgument;
            result.diagnostics = "Invalid locale handle";
            return result;
        }
        auto *slot = ResolveSlot(locale);
        if (!slot) {
            result.status = StatusCode::NotFound;
            result.diagnostics = "Locale not found";
            return result;
        }
        auto &record = slot->record;
        TouchLocale(record);
        m_Metrics.dateFormatRequests += 1;

        DateFormatterDescriptor descriptor;
        descriptor.dateStyle = options.dateStyle;
        descriptor.timeStyle = options.timeStyle;
        descriptor.timeZone = options.timeZone;
        descriptor.fractionalSeconds = options.fractionalSeconds;

        auto signature = ComputeDateSignature(locale, descriptor);
        std::uint64_t key = signature;
        DateFormatterCacheEntry *entry = nullptr;
        for (int attempt = 0; attempt < 4; ++attempt) {
            auto found = m_DateCache.find(key);
            if (found == m_DateCache.end()) {
                break;
            }
            if (found->second.locale == locale
                && found->second.generation == ExtractGeneration(locale)
                && found->second.descriptor.Equals(descriptor)) {
                entry = &found->second;
                entry->hits += 1;
                entry->lastUsedFrame = m_CurrentFrame;
                m_Metrics.dateFormatterHits += 1;
                break;
            }
            key ^= (kFnvPrime + static_cast<std::uint64_t>(attempt + 1));
        }
        if (!entry) {
            auto inserted = m_DateCache.emplace(key, DateFormatterCacheEntry());
            entry = &inserted.first->second;
            entry->descriptor = descriptor;
            entry->locale = locale;
            entry->generation = ExtractGeneration(locale);
            entry->signature = key;
            entry->hits = 0;
            entry->lastUsedFrame = m_CurrentFrame;
            if (m_DateCache.size() > kDateCacheLimit) {
                EvictStaleFormatters();
            }
        }

        result.value = FormatDateTimeInternal(record, descriptor, value);
        result.status = StatusCode::Ok;
        result.locale = locale;
        result.localeVersion = record.version;
        result.formatterVersion = entry->signature;
        m_Metrics.dateCacheSize = m_DateCache.size();
        return result;
    }

    IntlModule::FormatResult IntlModule::FormatDateTime(std::string_view identifier,
                                                        std::chrono::system_clock::time_point value,
                                                        const DateTimeFormatOptions &options) {
        LocaleHandle handle = kInvalidLocale;
        StatusCode status = EnsureLocale(identifier, handle);
        if (status != StatusCode::Ok) {
            FormatResult result;
            result.status = status;
            result.diagnostics = "Locale ensure failed";
            return result;
        }
        return FormatDateTime(handle, value, options);
    }
    IntlModule::FormatResult IntlModule::FormatList(LocaleHandle locale,
                                std::span<const std::string_view> values,
                                const ListFormatOptions &options) {
        std::lock_guard<std::mutex> lock(m_Mutex);
        FormatResult result;
        if (locale == kInvalidLocale) {
            result.status = StatusCode::InvalidArgument;
            result.diagnostics = "Invalid locale handle";
            return result;
        }
        auto *slot = ResolveSlot(locale);
        if (!slot) {
            result.status = StatusCode::NotFound;
            result.diagnostics = "Locale not found";
            return result;
        }
        auto &record = slot->record;
        TouchLocale(record);
        m_Metrics.listFormatRequests += 1;

        ListFormatterDescriptor descriptor;
        descriptor.type = options.type;
        descriptor.style = options.style;

        auto signature = ComputeListSignature(locale, descriptor);
        std::uint64_t key = signature;
        ListFormatterCacheEntry *entry = nullptr;
        for (int attempt = 0; attempt < 4; ++attempt) {
            auto found = m_ListCache.find(key);
            if (found == m_ListCache.end()) {
                break;
            }
            if (found->second.locale == locale
                && found->second.generation == ExtractGeneration(locale)
                && found->second.descriptor.Equals(descriptor)) {
                entry = &found->second;
                entry->hits += 1;
                entry->lastUsedFrame = m_CurrentFrame;
                m_Metrics.listFormatterHits += 1;
                break;
            }
            key ^= (kFnvPrime + static_cast<std::uint64_t>(attempt + 1));
        }
        if (!entry) {
            auto inserted = m_ListCache.emplace(key, ListFormatterCacheEntry());
            entry = &inserted.first->second;
            entry->descriptor = descriptor;
            entry->locale = locale;
            entry->generation = ExtractGeneration(locale);
            entry->signature = key;
            entry->hits = 0;
            entry->lastUsedFrame = m_CurrentFrame;
            if (m_ListCache.size() > kListCacheLimit) {
                EvictStaleFormatters();
            }
        }

        result.value = FormatListInternal(record, descriptor, values);
        result.status = StatusCode::Ok;
        result.locale = locale;
        result.localeVersion = record.version;
        result.formatterVersion = entry->signature;
        m_Metrics.listCacheSize = m_ListCache.size();
        return result;
    }
    IntlModule::FormatResult IntlModule::FormatList(LocaleHandle locale,
                                std::initializer_list<std::string_view> values,
                                const ListFormatOptions &options) {
        return FormatList(locale, std::span<const std::string_view>(values.begin(), values.size()), options);
    }
    IntlModule::FormatResult IntlModule::FormatList(std::string_view identifier,
                                std::span<const std::string_view> values,
                                const ListFormatOptions &options) {
        LocaleHandle handle = kInvalidLocale;
        StatusCode status = EnsureLocale(identifier, handle);
        if (status != StatusCode::Ok) {
            FormatResult result;
            result.status = status;
            result.diagnostics = "Locale ensure failed";
            return result;
        }
        return FormatList(handle, values, options);
    }
    IntlModule::FormatResult IntlModule::FormatList(std::string_view identifier,
                                std::initializer_list<std::string_view> values,
                                const ListFormatOptions &options) {
        return FormatList(identifier, std::span<const std::string_view>(values.begin(), values.size()), options);
    }
    IntlModule::LocaleSnapshot IntlModule::Snapshot(LocaleHandle locale) const {
        std::lock_guard<std::mutex> lock(m_Mutex);
        LocaleSnapshot snapshot;
        if (locale == kInvalidLocale) {
            return snapshot;
        }
        const auto *slot = ResolveSlot(locale);
        if (!slot) {
            return snapshot;
        }
        const auto &record = slot->record;
        snapshot.handle = record.handle;
        snapshot.identifier = record.identifier;
        snapshot.currencyCode = record.data.currencyCode;
        snapshot.currencySymbol = record.data.currencySymbol;
        snapshot.decimalSeparator = record.data.decimalSeparator;
        snapshot.groupSeparator = record.data.groupSeparator;
        snapshot.grouping = record.data.grouping;
        snapshot.version = record.version;
        snapshot.active = record.active;
        return snapshot;
    }

    void IntlModule::Clear(bool releaseCapacity) {
        std::lock_guard<std::mutex> lock(m_Mutex);
        ClearLocked(releaseCapacity);
    }

    void IntlModule::ClearLocked(bool releaseCapacity) {
        for (std::size_t i = 0; i < m_Slots.size(); ++i) {
            auto &slot = m_Slots[i];
            if (!slot.inUse) {
                continue;
            }
            slot.inUse = false;
            slot.record = LocaleRecord();
            slot.generation = NextGeneration(slot.generation);
            m_FreeList.push_back(static_cast<std::uint32_t>(i));
        }
        m_LocaleLookup.clear();
        m_NumberCache.clear();
        m_DateCache.clear();
        m_ListCache.clear();
        m_Metrics.numberCacheSize = 0;
        m_Metrics.dateCacheSize = 0;
        m_Metrics.listCacheSize = 0;
        m_DefaultLocaleHandle = kInvalidLocale;
        if (releaseCapacity) {
            m_Slots.clear();
            m_FreeList.clear();
        }
    }

    const IntlModule::Metrics &IntlModule::GetMetrics() const noexcept {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_Metrics;
    }

    std::string IntlModule::DefaultLocale() const noexcept {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_DefaultLocale;
    }

    void IntlModule::SetDefaultLocale(std::string_view identifier) {
        std::lock_guard<std::mutex> lock(m_Mutex);
        LocaleHandle handle = kInvalidLocale;
        if (EnsureLocaleLocked(identifier, handle) == StatusCode::Ok) {
            auto *slot = ResolveSlot(handle);
            if (slot) {
                m_DefaultLocale = slot->record.identifier;
                m_DefaultLocaleHandle = handle;
            }
        }
    }
    IntlModule::LocaleHandle IntlModule::MakeHandle(std::size_t index, std::uint16_t generation) noexcept {
        return static_cast<LocaleHandle>((static_cast<std::uint32_t>(generation) << kHandleIndexBits)
                                         | static_cast<std::uint32_t>(index & kHandleIndexMask));
    }

    std::size_t IntlModule::ExtractIndex(LocaleHandle handle) noexcept {
        return static_cast<std::size_t>(handle & kHandleIndexMask);
    }

    std::uint16_t IntlModule::ExtractGeneration(LocaleHandle handle) noexcept {
        return static_cast<std::uint16_t>(handle >> kHandleIndexBits);
    }

    std::uint16_t IntlModule::NextGeneration(std::uint16_t generation) noexcept {
        return generation == 0xffffu ? static_cast<std::uint16_t>(1u) : static_cast<std::uint16_t>(generation + 1u);
    }

    IntlModule::LocaleSlot *IntlModule::ResolveSlot(LocaleHandle handle) noexcept {
        auto index = ExtractIndex(handle);
        if (index >= m_Slots.size()) {
            return nullptr;
        }
        auto &slot = m_Slots[index];
        if (!slot.inUse) {
            return nullptr;
        }
        if (ExtractGeneration(handle) != slot.generation) {
            return nullptr;
        }
        return &slot;
    }

    const IntlModule::LocaleSlot *IntlModule::ResolveSlot(LocaleHandle handle) const noexcept {
        auto index = ExtractIndex(handle);
        if (index >= m_Slots.size()) {
            return nullptr;
        }
        const auto &slot = m_Slots[index];
        if (!slot.inUse) {
            return nullptr;
        }
        if (ExtractGeneration(handle) != slot.generation) {
            return nullptr;
        }
        return &slot;
    }

    std::size_t IntlModule::AcquireSlot() {
        if (!m_FreeList.empty()) {
            auto index = m_FreeList.back();
            m_FreeList.pop_back();
            return index;
        }
        m_Slots.emplace_back();
        return m_Slots.size() - 1;
    }

    void IntlModule::ReleaseSlot(std::size_t index) noexcept {
        if (index >= m_Slots.size()) {
            return;
        }
        auto &slot = m_Slots[index];
        slot.inUse = false;
        slot.generation = NextGeneration(slot.generation);
        m_FreeList.push_back(static_cast<std::uint32_t>(index));
    }

    void IntlModule::InstallBuiltinLocales() {
        LocaleBlueprint us;
        us.identifier = "en-US";
        us.timePatternMedium = "h:mm:ss a";
        LocaleHandle handle = kInvalidLocale;
        (void) RegisterLocaleLocked(us, handle);
        m_DefaultLocaleHandle = handle;
        m_DefaultLocale = std::string(us.identifier);

        LocaleBlueprint gb;
        gb.identifier = "en-GB";
        gb.datePatternShort = "dd/MM/yy";
        gb.datePatternMedium = "dd MMM yyyy";
        gb.datePatternLong = "d MMMM yyyy";
        gb.timePatternShort = "HH:mm";
        gb.timePatternMedium = "HH:mm:ss";
        gb.timePatternLong = "HH:mm:ss";
        gb.currencyCode = "GBP";
        gb.currencySymbol = "GBP";
        gb.currencyPositivePattern = "{symbol}{value}";
        gb.currencyNegativePattern = "-{symbol}{value}";
        (void) RegisterLocaleLocked(gb, handle);

        LocaleBlueprint ru;
        ru.identifier = "ru-RU";
        ru.decimalSeparator = ',';
        ru.groupSeparator = ' ';
        ru.grouping = {3, 0, 0};
        ru.currencyCode = "RUB";
        ru.currencySymbol = "RUB";
        ru.positivePattern = "{value}";
        ru.negativePattern = "-{value}";
        ru.currencyPositivePattern = "{value} {symbol}";
        ru.currencyNegativePattern = "-{value} {symbol}";
        ru.percentPositivePattern = "{value}%";
        ru.percentNegativePattern = "-{value}%";
        ru.datePatternShort = "dd.MM.yyyy";
        ru.datePatternMedium = "d MMM yyyy";
        ru.datePatternLong = "d MMMM yyyy";
        ru.timePatternShort = "HH:mm";
        ru.timePatternMedium = "HH:mm:ss";
        ru.timePatternLong = "HH:mm:ss";
        ru.dateTimeSeparator = " ";
        ru.amDesignator = "";
        ru.pmDesignator = "";
        ru.monthNamesShort = {"yan", "feb", "mar", "apr", "may", "iun", "iul", "avg", "sen", "okt", "nov", "dek"};
        ru.monthNamesLong = {"yanvar", "fevral", "mart", "aprel", "mai", "iyun", "iyul", "avgust", "sentabr", "oktabr", "noyabr", "dekabr"};
        ru.weekdayNamesShort = {"vsk", "pon", "vto", "sre", "chet", "pat", "sub"};
        ru.weekdayNamesLong = {"voskresene", "ponedelnik", "vtornik", "sreda", "chetverg", "pyatnitsa", "subbota"};
        ru.listPatterns[EncodeListPatternIndex(IntlModule::ListType::Conjunction, IntlModule::ListStyle::Long)] =
            {"{0} i {1}", "{0}, {1}", "{0}, {1}", "{0}, i {1}"};
        ru.listPatterns[EncodeListPatternIndex(IntlModule::ListType::Conjunction, IntlModule::ListStyle::Short)] =
            {"{0} i {1}", "{0}, {1}", "{0}, {1}", "{0}, i {1}"};
        ru.listPatterns[EncodeListPatternIndex(IntlModule::ListType::Conjunction, IntlModule::ListStyle::Narrow)] =
            {"{0} i {1}", "{0}, {1}", "{0}, {1}", "{0}, i {1}"};
        ru.listPatterns[EncodeListPatternIndex(IntlModule::ListType::Disjunction, IntlModule::ListStyle::Long)] =
            {"{0} ili {1}", "{0}, {1}", "{0}, {1}", "{0}, ili {1}"};
        ru.listPatterns[EncodeListPatternIndex(IntlModule::ListType::Disjunction, IntlModule::ListStyle::Short)] =
            {"{0} ili {1}", "{0}, {1}", "{0}, {1}", "{0}, ili {1}"};
        ru.listPatterns[EncodeListPatternIndex(IntlModule::ListType::Disjunction, IntlModule::ListStyle::Narrow)] =
            {"{0} ili {1}", "{0}, {1}", "{0}, {1}", "{0}, ili {1}"};
        ru.listPatterns[EncodeListPatternIndex(IntlModule::ListType::Unit, IntlModule::ListStyle::Long)] =
            {"{0}, {1}", "{0}, {1}", "{0}, {1}", "{0}, {1}"};
        ru.listPatterns[EncodeListPatternIndex(IntlModule::ListType::Unit, IntlModule::ListStyle::Short)] =
            {"{0}, {1}", "{0}, {1}", "{0}, {1}", "{0}, {1}"};
        ru.listPatterns[EncodeListPatternIndex(IntlModule::ListType::Unit, IntlModule::ListStyle::Narrow)] =
            {"{0} {1}", "{0} {1}", "{0} {1}", "{0} {1}"};
        (void) RegisterLocaleLocked(ru, handle);
    }

    void IntlModule::ApplyBlueprint(LocaleRecord &record, const LocaleBlueprint &blueprint) {
        record.identifier.assign(blueprint.identifier.begin(), blueprint.identifier.end());
        record.data.decimalSeparator = blueprint.decimalSeparator;
        record.data.groupSeparator = blueprint.groupSeparator;
        record.data.grouping = blueprint.grouping;
        record.data.currencyCode.assign(blueprint.currencyCode.begin(), blueprint.currencyCode.end());
        record.data.currencySymbol.assign(blueprint.currencySymbol.begin(), blueprint.currencySymbol.end());
        if (record.data.currencySymbol.empty()) {
            record.data.currencySymbol = record.data.currencyCode;
        }
        record.data.percentSymbol.assign(blueprint.percentSymbol.begin(), blueprint.percentSymbol.end());
        record.data.nanSymbol.assign(blueprint.nanSymbol.begin(), blueprint.nanSymbol.end());
        record.data.infinitySymbol.assign(blueprint.infinitySymbol.begin(), blueprint.infinitySymbol.end());
        record.data.positivePattern.assign(blueprint.positivePattern.begin(), blueprint.positivePattern.end());
        record.data.negativePattern.assign(blueprint.negativePattern.begin(), blueprint.negativePattern.end());
        record.data.currencyPositivePattern.assign(blueprint.currencyPositivePattern.begin(),
                                                  blueprint.currencyPositivePattern.end());
        record.data.currencyNegativePattern.assign(blueprint.currencyNegativePattern.begin(),
                                                  blueprint.currencyNegativePattern.end());
        record.data.percentPositivePattern.assign(blueprint.percentPositivePattern.begin(),
                                                 blueprint.percentPositivePattern.end());
        record.data.percentNegativePattern.assign(blueprint.percentNegativePattern.begin(),
                                                 blueprint.percentNegativePattern.end());
        for (std::size_t i = 0; i < blueprint.monthNamesShort.size(); ++i) {
            record.data.monthNamesShort[i].assign(blueprint.monthNamesShort[i].begin(),
                                                  blueprint.monthNamesShort[i].end());
            record.data.monthNamesLong[i].assign(blueprint.monthNamesLong[i].begin(),
                                                 blueprint.monthNamesLong[i].end());
        }
        for (std::size_t i = 0; i < blueprint.weekdayNamesShort.size(); ++i) {
            record.data.weekdayNamesShort[i].assign(blueprint.weekdayNamesShort[i].begin(),
                                                    blueprint.weekdayNamesShort[i].end());
            record.data.weekdayNamesLong[i].assign(blueprint.weekdayNamesLong[i].begin(),
                                                   blueprint.weekdayNamesLong[i].end());
        }
        record.data.datePatterns[0].assign(blueprint.datePatternShort.begin(), blueprint.datePatternShort.end());
        record.data.datePatterns[1].assign(blueprint.datePatternMedium.begin(), blueprint.datePatternMedium.end());
        record.data.datePatterns[2].assign(blueprint.datePatternLong.begin(), blueprint.datePatternLong.end());
        record.data.timePatterns[0].assign(blueprint.timePatternShort.begin(), blueprint.timePatternShort.end());
        record.data.timePatterns[1].assign(blueprint.timePatternMedium.begin(), blueprint.timePatternMedium.end());
        record.data.timePatterns[2].assign(blueprint.timePatternLong.begin(), blueprint.timePatternLong.end());
        record.data.dateTimeSeparator.assign(blueprint.dateTimeSeparator.begin(), blueprint.dateTimeSeparator.end());
        record.data.amDesignator.assign(blueprint.amDesignator.begin(), blueprint.amDesignator.end());
        record.data.pmDesignator.assign(blueprint.pmDesignator.begin(), blueprint.pmDesignator.end());
        for (std::size_t i = 0; i < kListPatternVariants; ++i) {
            const auto &source = blueprint.listPatterns[i];
            auto &target = record.data.listPatterns[i];
            target.pair.assign(source.pair.begin(), source.pair.end());
            target.start.assign(source.start.begin(), source.start.end());
            target.middle.assign(source.middle.begin(), source.middle.end());
            target.end.assign(source.end.begin(), source.end.end());
        }
    }

    void IntlModule::TouchLocale(LocaleRecord &record) noexcept {
        record.lastUsedFrame = m_CurrentFrame;
        record.active = true;
    }
    StatusCode IntlModule::RegisterLocaleLocked(const LocaleBlueprint &blueprint, LocaleHandle &outHandle) {
        if (blueprint.identifier.empty()) {
            outHandle = kInvalidLocale;
            return StatusCode::InvalidArgument;
        }
        auto existing = m_LocaleLookup.find(blueprint.identifier);
        if (existing != m_LocaleLookup.end()) {
            auto index = ExtractIndex(existing->second);
            if (index < m_Slots.size()) {
                auto &slot = m_Slots[index];
                if (slot.inUse && slot.generation == ExtractGeneration(existing->second)) {
                    ApplyBlueprint(slot.record, blueprint);
                    slot.record.version += 1;
                    PurgeCachesForLocale(slot.record.handle);
                    m_Metrics.localesUpdated += 1;
                    outHandle = slot.record.handle;
                    return StatusCode::Ok;
                }
            }
        }
        auto index = AcquireSlot();
        if (index >= m_Slots.size()) {
            outHandle = kInvalidLocale;
            return StatusCode::InternalError;
        }
        auto &slot = m_Slots[index];
        slot.inUse = true;
        slot.record = LocaleRecord();
        slot.record.handle = MakeHandle(index, slot.generation);
        ApplyBlueprint(slot.record, blueprint);
        slot.record.version = 1;
        slot.record.active = true;
        m_LocaleLookup.emplace(slot.record.identifier, slot.record.handle);
        m_Metrics.localesRegistered += 1;
        outHandle = slot.record.handle;
        return StatusCode::Ok;
    }

    StatusCode IntlModule::EnsureLocaleLocked(std::string_view identifier, LocaleHandle &outHandle) {
        if (identifier.empty()) {
            outHandle = kInvalidLocale;
            return StatusCode::InvalidArgument;
        }
        auto it = m_LocaleLookup.find(identifier);
        if (it != m_LocaleLookup.end()) {
            outHandle = it->second;
            return StatusCode::Ok;
        }
        LocaleBlueprint fallback;
        fallback.identifier = identifier;
        fallback.currencyCode = fallback.currencyCode.empty() ? "USD" : fallback.currencyCode;
        fallback.currencySymbol = fallback.currencySymbol.empty() ? fallback.currencyCode : fallback.currencySymbol;
        return RegisterLocaleLocked(fallback, outHandle);
    }

    void IntlModule::PurgeCachesForLocale(LocaleHandle handle) {
        for (auto it = m_NumberCache.begin(); it != m_NumberCache.end();) {
            if (it->second.locale == handle) {
                it = m_NumberCache.erase(it);
                m_Metrics.cacheEvictions += 1;
            } else {
                ++it;
            }
        }
        for (auto it = m_DateCache.begin(); it != m_DateCache.end();) {
            if (it->second.locale == handle) {
                it = m_DateCache.erase(it);
                m_Metrics.cacheEvictions += 1;
            } else {
                ++it;
            }
        }
        for (auto it = m_ListCache.begin(); it != m_ListCache.end();) {
            if (it->second.locale == handle) {
                it = m_ListCache.erase(it);
                m_Metrics.cacheEvictions += 1;
            } else {
                ++it;
            }
        }
        m_Metrics.numberCacheSize = m_NumberCache.size();
        m_Metrics.dateCacheSize = m_DateCache.size();
        m_Metrics.listCacheSize = m_ListCache.size();
    }

    std::uint64_t IntlModule::ComputeNumberSignature(LocaleHandle locale,
                                                     const NumberFormatterDescriptor &descriptor) const noexcept {
        std::uint64_t hash = HashString(descriptor.currencyCode);
        hash ^= HashString(descriptor.currencySymbol) + 0x9e3779b97f4a7c15ULL
                + static_cast<std::uint64_t>(descriptor.style) * kFnvPrime
                + (descriptor.useGrouping ? 0x847cab1dULL : 0x23984d5fULL);
        hash ^= static_cast<std::uint64_t>(descriptor.signDisplay) * 0x100000001b3ULL;
        hash ^= static_cast<std::uint64_t>(descriptor.currencyDisplay) * 0x100001b3ULL;
        hash ^= static_cast<std::uint64_t>(descriptor.notation) * 0x1b3ULL;
        hash ^= static_cast<std::uint64_t>(descriptor.minFractionDigits) << 8;
        hash ^= static_cast<std::uint64_t>(descriptor.maxFractionDigits) << 12;
        hash ^= static_cast<std::uint64_t>(locale) << 1;
        return hash;
    }

    std::uint64_t IntlModule::ComputeDateSignature(LocaleHandle locale,
                                                   const DateFormatterDescriptor &descriptor) const noexcept {
        std::uint64_t hash = static_cast<std::uint64_t>(descriptor.dateStyle);
        hash ^= static_cast<std::uint64_t>(descriptor.timeStyle) << 8;
        hash ^= static_cast<std::uint64_t>(descriptor.timeZone) << 16;
        hash ^= static_cast<std::uint64_t>(descriptor.fractionalSeconds ? 1 : 0) << 24;
        hash ^= static_cast<std::uint64_t>(locale) << 1;
        return hash * kFnvPrime;
    }

    std::uint64_t IntlModule::ComputeListSignature(LocaleHandle locale,
                                                  const ListFormatterDescriptor &descriptor) const noexcept {
        std::uint64_t hash = static_cast<std::uint64_t>(descriptor.type) * 0x9e3779b97f4a7c15ULL;
        hash ^= static_cast<std::uint64_t>(descriptor.style) * kFnvPrime;
        hash ^= static_cast<std::uint64_t>(locale) << 3;
        return hash;
    }

    std::string IntlModule::FormatNumberInternal(const LocaleRecord &record,
                                                 const NumberFormatterDescriptor &descriptor,
                                                 double value) const {
        const auto &data = record.data;
        if (std::isnan(value)) {
            return data.nanSymbol;
        }
        if (std::isinf(value)) {
            std::string result;
            result.reserve(data.infinitySymbol.size() + 1);
            if (std::signbit(value)) {
                result.push_back('-');
            } else if (descriptor.signDisplay == SignDisplay::Always) {
                result.push_back('+');
            }
            result.append(data.infinitySymbol);
            return result;
        }

        double working = value;
        bool negative = std::signbit(working);
        working = std::fabs(working);
        if (descriptor.style == NumberStyle::Percent) {
            working *= 100.0;
        }
        char buffer[128];
        auto [ptr, ec] = std::to_chars(buffer, buffer + sizeof(buffer), working, std::chars_format::fixed,
                                       static_cast<int>(descriptor.maxFractionDigits));
        if (ec != std::errc()) {
            std::snprintf(buffer, sizeof(buffer), "%.*f",
                          static_cast<int>(descriptor.maxFractionDigits), working);
            ptr = buffer + std::strlen(buffer);
        }
        std::string digits(buffer, ptr - buffer);
        std::string integerPart;
        std::string fractionPart;
        auto dot = digits.find('.');
        if (dot == std::string::npos) {
            integerPart = digits;
        } else {
            integerPart.assign(digits.data(), dot);
            fractionPart.assign(digits.data() + dot + 1, digits.size() - dot - 1);
        }
        while (fractionPart.size() > descriptor.minFractionDigits && !fractionPart.empty()
               && fractionPart.back() == '0') {
            fractionPart.pop_back();
        }
        while (fractionPart.size() < descriptor.minFractionDigits) {
            fractionPart.push_back('0');
        }
        if (integerPart.empty()) {
            integerPart = "0";
        }
        if (descriptor.useGrouping) {
            ApplyGrouping(integerPart, data.groupSeparator, data.grouping);
        }
        std::string core = std::move(integerPart);
        if (!fractionPart.empty()) {
            core.push_back(data.decimalSeparator);
            core.append(fractionPart);
        }

        if (!negative) {
            bool shouldPrefix = false;
            if (descriptor.signDisplay == SignDisplay::Always) {
                shouldPrefix = true;
            } else if (descriptor.signDisplay == SignDisplay::ExceptZero && !IsZero(value)) {
                shouldPrefix = true;
            }
            if (shouldPrefix) {
                core.insert(core.begin(), '+');
            }
        }

        std::string result;
        result.reserve(core.size() + 8);
        if (negative && descriptor.signDisplay == SignDisplay::Never) {
            negative = false;
        }
        if (descriptor.style == NumberStyle::Decimal) {
            auto pattern = negative ? data.negativePattern : data.positivePattern;
            AppendPattern(pattern, core, {}, {}, result);
        } else if (descriptor.style == NumberStyle::Currency) {
            auto pattern = negative ? data.currencyNegativePattern : data.currencyPositivePattern;
            AppendPattern(pattern,
                          core,
                          descriptor.currencySymbol.empty() ? data.currencySymbol : descriptor.currencySymbol,
                          descriptor.currencyCode.empty() ? data.currencyCode : descriptor.currencyCode,
                          result);
        } else {
            auto pattern = negative ? data.percentNegativePattern : data.percentPositivePattern;
            AppendPattern(pattern, core, data.percentSymbol, data.percentSymbol, result);
        }

        if (negative && descriptor.signDisplay == SignDisplay::Always && (result.empty() || result[0] != '-')) {
            result.insert(result.begin(), '-');
        }
        return result;
    }

    std::string IntlModule::FormatDateTimeInternal(const LocaleRecord &record,
                                                   const DateFormatterDescriptor &descriptor,
                                                   std::chrono::system_clock::time_point value) const {
        const auto &data = record.data;
        bool wantDate = descriptor.dateStyle != DateStyle::None;
        bool wantTime = descriptor.timeStyle != TimeStyle::None;

        std::time_t seconds = std::chrono::system_clock::to_time_t(value);
        std::tm tmValue{};
        if (!ConvertTime(seconds, descriptor.timeZone, tmValue)) {
            return std::string();
        }
        auto truncated = std::chrono::system_clock::from_time_t(seconds);
        auto fractional = value - truncated;
        auto millis = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(fractional).count());
        if (millis < 0) {
            millis += 1000;
        }

        std::string datePart;
        std::string timePart;
        bool fraction = descriptor.fractionalSeconds;

        if (wantDate) {
            std::size_t index = 0;
            switch (descriptor.dateStyle) {
                case DateStyle::Short: index = 0; break;
                case DateStyle::Medium: index = 1; break;
                case DateStyle::Long: index = 2; break;
                default: index = 0; break;
            }
            const auto &pattern = data.datePatterns[std::min<std::size_t>(index, data.datePatterns.size() - 1)];
            bool noFraction = false;
            datePart = FormatPattern(data, tmValue, pattern, noFraction, millis);
        }
        if (wantTime) {
            std::size_t index = 0;
            switch (descriptor.timeStyle) {
                case TimeStyle::Short: index = 0; break;
                case TimeStyle::Medium: index = 1; break;
                case TimeStyle::Long: index = 2; break;
                default: index = 0; break;
            }
            const auto &pattern = data.timePatterns[std::min<std::size_t>(index, data.timePatterns.size() - 1)];
            timePart = FormatPattern(data, tmValue, pattern, fraction, millis);
        }

        if (wantDate && wantTime) {
            std::string result;
            result.reserve(datePart.size() + timePart.size() + data.dateTimeSeparator.size());
            result.append(datePart);
            result.append(data.dateTimeSeparator);
            result.append(timePart);
            return result;
        }
        if (wantDate) {
            return datePart;
        }
        if (wantTime) {
            return timePart;
        }
        return std::string();
    }

    std::string IntlModule::FormatListInternal(const LocaleRecord &record,
                                       const ListFormatterDescriptor &descriptor,
                                       std::span<const std::string_view> values) const {
        if (values.empty()) {
            return std::string();
        }
        const auto index = EncodeListPatternIndex(descriptor.type, descriptor.style);
        const auto &patterns = record.data.listPatterns[index];
        if (values.size() == 1) {
            return std::string(values.front());
        }
        if (values.size() == 2) {
            std::string result;
            result.reserve(patterns.pair.size() + values[0].size() + values[1].size());
            ApplyListPattern(patterns.pair, values[0], values[1], result);
            return result;
        }
        std::string result(values.front());
        for (std::size_t i = 1; i < values.size(); ++i) {
            const std::string &pattern = (i == 1) ? patterns.start
                                        : (i == values.size() - 1 ? patterns.end : patterns.middle);
            std::string combined;
            combined.reserve(pattern.size() + result.size() + values[i].size());
            ApplyListPattern(pattern, std::string_view(result), values[i], combined);
            result.swap(combined);
        }
        return result;
    }

    void IntlModule::EvictStaleFormatters() {
        const std::uint64_t cutoff = m_CurrentFrame > kCacheHorizonFrames
                                         ? m_CurrentFrame - kCacheHorizonFrames
                                         : 0;
        for (auto it = m_NumberCache.begin(); it != m_NumberCache.end();) {
            if (it->second.lastUsedFrame <= cutoff) {
                it = m_NumberCache.erase(it);
                m_Metrics.cacheEvictions += 1;
            } else {
                ++it;
            }
        }
        for (auto it = m_DateCache.begin(); it != m_DateCache.end();) {
            if (it->second.lastUsedFrame <= cutoff) {
                it = m_DateCache.erase(it);
                m_Metrics.cacheEvictions += 1;
            } else {
                ++it;
            }
        }
        for (auto it = m_ListCache.begin(); it != m_ListCache.end();) {
            if (it->second.lastUsedFrame <= cutoff) {
                it = m_ListCache.erase(it);
                m_Metrics.cacheEvictions += 1;
            } else {
                ++it;
            }
        }
        if (m_NumberCache.size() > kNumberCacheLimit) {
            auto it = std::min_element(m_NumberCache.begin(), m_NumberCache.end(), [](const auto &lhs, const auto &rhs) {
                return lhs.second.lastUsedFrame < rhs.second.lastUsedFrame;
            });
            if (it != m_NumberCache.end()) {
                m_NumberCache.erase(it);
                m_Metrics.cacheEvictions += 1;
            }
        }
        if (m_DateCache.size() > kDateCacheLimit) {
            auto it = std::min_element(m_DateCache.begin(), m_DateCache.end(), [](const auto &lhs, const auto &rhs) {
                return lhs.second.lastUsedFrame < rhs.second.lastUsedFrame;
            });
            if (it != m_DateCache.end()) {
                m_DateCache.erase(it);
                m_Metrics.cacheEvictions += 1;
            }
        }
        if (m_ListCache.size() > kListCacheLimit) {
            auto it = std::min_element(m_ListCache.begin(), m_ListCache.end(), [](const auto &lhs, const auto &rhs) {
                return lhs.second.lastUsedFrame < rhs.second.lastUsedFrame;
            });
            if (it != m_ListCache.end()) {
                m_ListCache.erase(it);
                m_Metrics.cacheEvictions += 1;
            }
        }
        m_Metrics.numberCacheSize = m_NumberCache.size();
        m_Metrics.dateCacheSize = m_DateCache.size();
        m_Metrics.listCacheSize = m_ListCache.size();
    }
}

