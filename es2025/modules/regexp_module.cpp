
#include "spectre/es2025/modules/regexp_module.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <regex>
#include <stdexcept>

#include "spectre/runtime.h"

namespace spectre::es2025 {
    namespace {
        constexpr std::string_view kName = "RegExp";
        constexpr std::string_view kSummary = "Regular expression compilation and execution engine hooks.";
        constexpr std::string_view kReference = "ECMA-262 Section 22.2";
        constexpr std::uint64_t kHandleSlotMask = 0xffffffffull;
        constexpr std::uint64_t kHotFrameWindow = 12;

        constexpr char kFlagOrder[] = {'g', 'i', 'm', 's', 'u', 'y'};
        constexpr std::uint32_t kNoMatch = std::numeric_limits<std::uint32_t>::max();

        std::string NormalizeFlags(const std::string &raw, const bool flags[6]) {
            std::string normalized;
            normalized.reserve(raw.size());
            for (char order : kFlagOrder) {
                switch (order) {
                    case 'g':
                        if (flags[0]) normalized.push_back('g');
                        break;
                    case 'i':
                        if (flags[1]) normalized.push_back('i');
                        break;
                    case 'm':
                        if (flags[2]) normalized.push_back('m');
                        break;
                    case 's':
                        if (flags[3]) normalized.push_back('s');
                        break;
                    case 'u':
                        if (flags[4]) normalized.push_back('u');
                        break;
                    case 'y':
                        if (flags[5]) normalized.push_back('y');
                        break;
                }
            }
            return normalized;
        }
    }

    RegExpModule::Metrics::Metrics() noexcept
        : compiled(0),
          cacheHits(0),
          cacheMisses(0),
          executions(0),
          matches(0),
          mismatches(0),
          replacements(0),
          splits(0),
          resets(0),
          activePatterns(0),
          hotPatterns(0),
          lastFrameTouched(0),
          gpuOptimized(false) {
    }

    RegExpModule::MatchRange::MatchRange() noexcept : begin(0), end(0) {
    }

    RegExpModule::MatchRange::MatchRange(std::uint32_t b, std::uint32_t e) noexcept : begin(b), end(e) {
    }

    RegExpModule::MatchResult::MatchResult() noexcept
        : matched(false), index(0), length(0), nextIndex(0), groups() {
    }

    RegExpModule::ParsedFlags::ParsedFlags() noexcept
        : global(false),
          ignoreCase(false),
          multiline(false),
          dotAll(false),
          unicode(false),
          sticky(false),
          normalized() {
    }

    RegExpModule::RegExpModule()
        : m_Runtime(nullptr),
          m_Subsystems(nullptr),
          m_Config{},
          m_GpuEnabled(false),
          m_Initialized(false),
          m_CurrentFrame(0),
          m_Metrics{},
          m_Slots{},
          m_FreeSlots{},
          m_Index() {
    }
    std::string_view RegExpModule::Name() const noexcept {
        return kName;
    }

    std::string_view RegExpModule::Summary() const noexcept {
        return kSummary;
    }

    std::string_view RegExpModule::SpecificationReference() const noexcept {
        return kReference;
    }

    void RegExpModule::Initialize(const ModuleInitContext &context) {
        m_Runtime = &context.runtime;
        m_Subsystems = &context.subsystems;
        m_Config = context.config;
        m_GpuEnabled = context.config.enableGpuAcceleration;
        m_Initialized = true;
        m_CurrentFrame = 0;
        m_Metrics = Metrics();
        m_Metrics.gpuOptimized = m_GpuEnabled;
        Reset();
    }

    void RegExpModule::Tick(const TickInfo &info, const ModuleTickContext &) noexcept {
        m_CurrentFrame = info.frameIndex;
        RecomputeHotMetrics();
    }

    void RegExpModule::OptimizeGpu(const ModuleGpuContext &context) noexcept {
        m_GpuEnabled = context.enableAcceleration;
        m_Metrics.gpuOptimized = m_GpuEnabled;
    }

    void RegExpModule::Reconfigure(const RuntimeConfig &config) {
        m_Config = config;
        m_GpuEnabled = config.enableGpuAcceleration;
        m_Metrics.gpuOptimized = m_GpuEnabled;
    }

    StatusCode RegExpModule::ParseFlags(std::string_view flags,
                                        ParsedFlags &outFlags,
                                        std::string &outNormalized) const {
        bool seen[6] = {false, false, false, false, false, false};
        std::string raw;
        raw.reserve(flags.size());
        for (char ch : flags) {
            switch (ch) {
                case 'g':
                    if (seen[0]) {
                        return StatusCode::InvalidArgument;
                    }
                    seen[0] = true;
                    outFlags.global = true;
                    raw.push_back('g');
                    break;
                case 'i':
                    if (seen[1]) {
                        return StatusCode::InvalidArgument;
                    }
                    seen[1] = true;
                    outFlags.ignoreCase = true;
                    raw.push_back('i');
                    break;
                case 'm':
                    if (seen[2]) {
                        return StatusCode::InvalidArgument;
                    }
                    seen[2] = true;
                    outFlags.multiline = true;
                    raw.push_back('m');
                    break;
                case 's':
                    if (seen[3]) {
                        return StatusCode::InvalidArgument;
                    }
                    seen[3] = true;
                    outFlags.dotAll = true;
                    raw.push_back('s');
                    break;
                case 'u':
                    if (seen[4]) {
                        return StatusCode::InvalidArgument;
                    }
                    seen[4] = true;
                    outFlags.unicode = true;
                    raw.push_back('u');
                    break;
                case 'y':
                    if (seen[5]) {
                        return StatusCode::InvalidArgument;
                    }
                    seen[5] = true;
                    outFlags.sticky = true;
                    raw.push_back('y');
                    break;
                default:
                    return StatusCode::InvalidArgument;
            }
        }
        outNormalized = NormalizeFlags(raw, seen);
        return StatusCode::Ok;
    }

    StatusCode RegExpModule::DecoratePattern(const ParsedFlags &flags,
                                             std::string_view pattern,
                                             std::string &outDecorated) const {
        outDecorated.clear();
        if (flags.multiline || flags.dotAll) {
            outDecorated.append("(?");
            if (flags.multiline) {
                outDecorated.push_back('m');
            }
            if (flags.dotAll) {
                outDecorated.push_back('s');
            }
            outDecorated.push_back(')');
        }
        outDecorated.append(pattern.begin(), pattern.end());
        return StatusCode::Ok;
    }
    StatusCode RegExpModule::Compile(std::string_view pattern, std::string_view flags, Handle &outHandle) {
        outHandle = 0;
        ParsedFlags parsed;
        std::string normalized;
        auto status = ParseFlags(flags, parsed, normalized);
        if (status != StatusCode::Ok) {
            return status;
        }
        std::string key;
        key.reserve(pattern.size() + normalized.size() + 1);
        key.append(pattern.begin(), pattern.end());
        key.push_back('\n');
        key.append(normalized);

        auto cached = m_Index.find(key);
        if (cached != m_Index.end()) {
            if (auto *record = FindMutable(cached->second)) {
                outHandle = record->handle;
                Touch(*record);
                m_Metrics.cacheHits += 1;
                return StatusCode::Ok;
            }
            m_Index.erase(cached);
        }

        std::string decorated;
        status = DecoratePattern(parsed, pattern, decorated);
        if (status != StatusCode::Ok) {
            return status;
        }
        std::regex_constants::syntax_option_type syntax = std::regex_constants::ECMAScript;
        if (parsed.ignoreCase) {
            syntax |= std::regex_constants::icase;
        }
        std::regex compiled;
        try {
            compiled = std::regex(decorated, syntax);
        } catch (const std::regex_error &) {
            return StatusCode::InvalidArgument;
        }

        std::uint32_t slotIndex = AcquireSlot();
        auto &slot = m_Slots[slotIndex];
        auto generation = slot.generation;
        auto handle = EncodeHandle(slotIndex, generation);
        slot.inUse = true;
        slot.record = PatternRecord();
        slot.record.handle = handle;
        slot.record.slot = slotIndex;
        slot.record.generation = generation;
        slot.record.pattern.assign(pattern.begin(), pattern.end());
        slot.record.flags = normalized;
        slot.record.decorated = decorated;
        slot.record.regex = std::move(compiled);
        slot.record.syntax = syntax;
        slot.record.global = parsed.global;
        slot.record.ignoreCase = parsed.ignoreCase;
        slot.record.multiline = parsed.multiline;
        slot.record.dotAll = parsed.dotAll;
        slot.record.unicode = parsed.unicode;
        slot.record.sticky = parsed.sticky;
        slot.record.lastIndex = 0;
        slot.record.hot = true;
        slot.record.lastTouchFrame = m_CurrentFrame;

        UpdateIndex(slot.record, true);
        outHandle = handle;
        m_Metrics.compiled += 1;
        m_Metrics.cacheMisses += 1;
        m_Metrics.activePatterns += 1;
        if (slot.record.hot) {
            m_Metrics.hotPatterns += 1;
        }
        m_Metrics.lastFrameTouched = m_CurrentFrame;
        return StatusCode::Ok;
    }

    StatusCode RegExpModule::Destroy(Handle handle) {
        auto *record = FindMutable(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        auto slotIndex = record->slot;
        if (record->hot && m_Metrics.hotPatterns > 0) {
            m_Metrics.hotPatterns -= 1;
        }
        if (m_Metrics.activePatterns > 0) {
            m_Metrics.activePatterns -= 1;
        }
        ReleaseSlot(slotIndex);
        m_Metrics.lastFrameTouched = m_CurrentFrame;
        return StatusCode::Ok;
    }

    StatusCode RegExpModule::ResetLastIndex(Handle handle) {
        auto *record = FindMutable(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        record->lastIndex = 0;
        Touch(*record);
        m_Metrics.resets += 1;
        return StatusCode::Ok;
    }

    std::size_t RegExpModule::LastIndex(Handle handle) const noexcept {
        const auto *record = Find(handle);
        return record ? record->lastIndex : 0;
    }

    StatusCode RegExpModule::Source(Handle handle, std::string &outPattern, std::string &outFlags) const {
        const auto *record = Find(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        outPattern = record->pattern;
        outFlags = record->flags;
        return StatusCode::Ok;
    }
    StatusCode RegExpModule::Exec(Handle handle, std::string_view input, MatchResult &outResult) {
    return Exec(handle, input, std::numeric_limits<std::size_t>::max(), outResult);
}

StatusCode RegExpModule::Exec(Handle handle,
                                  std::string_view input,
                                  std::size_t startIndex,
                                  MatchResult &outResult) {
        outResult = MatchResult();
        auto *record = FindMutable(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        std::size_t effectiveStart = startIndex;
        if (effectiveStart == std::numeric_limits<std::size_t>::max()) {
            effectiveStart = (record->global || record->sticky) ? record->lastIndex : 0;
        }
        if (effectiveStart > input.size()) {
            if (record->global || record->sticky) {
                record->lastIndex = 0;
            }
            outResult.matched = false;
            outResult.index = static_cast<std::uint32_t>(std::min<std::size_t>(effectiveStart, std::numeric_limits<std::uint32_t>::max()));
            outResult.length = 0;
            outResult.nextIndex = static_cast<std::uint32_t>(std::min<std::size_t>(record->lastIndex, std::numeric_limits<std::uint32_t>::max()));
            m_Metrics.executions += 1;
            m_Metrics.mismatches += 1;
            Touch(*record);
            return StatusCode::Ok;
        }

        auto beginIt = input.begin();
        auto searchBegin = beginIt + effectiveStart;
        std::match_results<std::string_view::const_iterator> matches;
        auto searchFlags = std::regex_constants::match_default;
        if (record->sticky) {
            searchFlags |= std::regex_constants::match_continuous;
        }
        bool matched = std::regex_search(searchBegin, input.end(), matches, record->regex, searchFlags);
        m_Metrics.executions += 1;
        if (!matched) {
            outResult.matched = false;
            outResult.index = static_cast<std::uint32_t>(std::min<std::size_t>(effectiveStart, std::numeric_limits<std::uint32_t>::max()));
            outResult.length = 0;
            if (record->global || record->sticky) {
                record->lastIndex = 0;
                outResult.nextIndex = 0;
            } else {
                outResult.nextIndex = static_cast<std::uint32_t>(std::min<std::size_t>(record->lastIndex, std::numeric_limits<std::uint32_t>::max()));
            }
            m_Metrics.mismatches += 1;
            Touch(*record);
            return StatusCode::Ok;
        }

        auto matchOffset = matches.position(0);
        auto matchLength = matches.length(0);
        auto absoluteIndex = effectiveStart + static_cast<std::size_t>(matchOffset);
        auto absoluteEnd = absoluteIndex + static_cast<std::size_t>(matchLength);
        if (record->global || record->sticky) {
            record->lastIndex = absoluteEnd;
        }
        outResult.matched = true;
        outResult.index = static_cast<std::uint32_t>(std::min<std::size_t>(absoluteIndex, std::numeric_limits<std::uint32_t>::max()));
        outResult.length = static_cast<std::uint32_t>(std::min<std::size_t>(matchLength, std::numeric_limits<std::uint32_t>::max()));
        outResult.nextIndex = static_cast<std::uint32_t>(std::min<std::size_t>(record->lastIndex, std::numeric_limits<std::uint32_t>::max()));
        outResult.groups.clear();
        outResult.groups.reserve(matches.size());
        for (std::size_t i = 0; i < matches.size(); ++i) {
            if (!matches[i].matched) {
                outResult.groups.emplace_back(kNoMatch, kNoMatch);
                continue;
            }
            auto groupStart = effectiveStart + static_cast<std::size_t>(matches.position(i));
            auto groupEnd = groupStart + static_cast<std::size_t>(matches.length(i));
            outResult.groups.emplace_back(
                static_cast<std::uint32_t>(std::min<std::size_t>(groupStart, std::numeric_limits<std::uint32_t>::max())),
                static_cast<std::uint32_t>(std::min<std::size_t>(groupEnd, std::numeric_limits<std::uint32_t>::max())));
        }
        m_Metrics.matches += 1;
        Touch(*record);
        return StatusCode::Ok;
    }

    StatusCode RegExpModule::Test(Handle handle, std::string_view input, bool &outMatched) {
        auto *record = FindMutable(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        MatchResult result;
        auto startIndex = (record->global || record->sticky) ? record->lastIndex : 0;
        auto status = Exec(handle, input, startIndex, result);
        if (status != StatusCode::Ok) {
            return status;
        }
        outMatched = result.matched;
        return StatusCode::Ok;
    }
    StatusCode RegExpModule::Replace(Handle handle,
                                     std::string_view input,
                                     std::string_view replacement,
                                     std::string &outOutput,
                                     bool allOccurrences) {
        auto *record = FindMutable(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        outOutput.clear();
        if (record->sticky) {
            MatchResult result;
            auto status = Exec(handle, input, std::numeric_limits<std::size_t>::max(), result);
            if (status != StatusCode::Ok) {
                return status;
            }
            if (!result.matched) {
                outOutput.assign(input.begin(), input.end());
                Touch(*record);
                return StatusCode::Ok;
            }
            outOutput.reserve(input.size() + replacement.size());
            outOutput.append(input.begin(), input.begin() + result.index);
            outOutput.append(replacement.begin(), replacement.end());
            outOutput.append(input.begin() + result.index + result.length, input.end());
            m_Metrics.replacements += 1;
            Touch(*record);
            return StatusCode::Ok;
        }

        std::regex_constants::match_flag_type formatFlags = std::regex_constants::format_default;
        if (!allOccurrences && !record->global) {
            formatFlags |= std::regex_constants::format_first_only;
        }
        try {
            std::string inputCopy(input.begin(), input.end());
            outOutput = std::regex_replace(inputCopy, record->regex, std::string(replacement), formatFlags);
        } catch (const std::regex_error &) {
            return StatusCode::InvalidArgument;
        }
        if (record->global) {
            record->lastIndex = 0;
        }
        m_Metrics.replacements += 1;
        Touch(*record);
        return StatusCode::Ok;
    }

    StatusCode RegExpModule::Split(Handle handle,
                                   std::string_view input,
                                   std::size_t limit,
                                   std::vector<std::string> &outParts) {
        auto *record = FindMutable(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        outParts.clear();
        if (limit == 0) {
            if (record->global || record->sticky) {
                record->lastIndex = 0;
            }
            Touch(*record);
            return StatusCode::Ok;
        }
        try {
            std::string inputCopy(input.begin(), input.end());
            std::regex_token_iterator<std::string::const_iterator> it(inputCopy.begin(), inputCopy.end(), record->regex, -1);
            std::regex_token_iterator<std::string::const_iterator> end;
            while (it != end) {
                if (limit > 0 && outParts.size() + 1 >= limit) {
                    outParts.emplace_back(it->first, inputCopy.cend());
                    break;
                }
                outParts.emplace_back(it->first, it->second);
                ++it;
            }
        } catch (const std::regex_error &) {
            return StatusCode::InvalidArgument;
        }
        if (record->global || record->sticky) {
            record->lastIndex = 0;
        }
        m_Metrics.splits += 1;
        Touch(*record);
        return StatusCode::Ok;
    }
    const RegExpModule::Metrics &RegExpModule::GetMetrics() const noexcept {
        return m_Metrics;
    }

    bool RegExpModule::GpuEnabled() const noexcept {
        return m_GpuEnabled;
    }

    RegExpModule::Handle RegExpModule::EncodeHandle(std::uint32_t slot, std::uint32_t generation) noexcept {
        return (static_cast<Handle>(generation) << 32) | static_cast<Handle>(slot);
    }

    std::uint32_t RegExpModule::DecodeSlot(Handle handle) noexcept {
        return static_cast<std::uint32_t>(handle & kHandleSlotMask);
    }

    std::uint32_t RegExpModule::DecodeGeneration(Handle handle) noexcept {
        return static_cast<std::uint32_t>((handle >> 32) & kHandleSlotMask);
    }

    RegExpModule::PatternRecord *RegExpModule::FindMutable(Handle handle) noexcept {
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
        return &slot.record;
    }

    const RegExpModule::PatternRecord *RegExpModule::Find(Handle handle) const noexcept {
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
        return &slot.record;
    }

    std::uint32_t RegExpModule::AcquireSlot() {
        if (!m_FreeSlots.empty()) {
            auto slotIndex = m_FreeSlots.back();
            m_FreeSlots.pop_back();
            auto &slot = m_Slots[slotIndex];
            slot.inUse = true;
            slot.generation += 1;
            return slotIndex;
        }
        auto slotIndex = static_cast<std::uint32_t>(m_Slots.size());
        m_Slots.emplace_back();
        auto &slot = m_Slots.back();
        slot.inUse = true;
        slot.generation = 1;
        return slotIndex;
    }

    void RegExpModule::ReleaseSlot(std::uint32_t slotIndex) {
        if (slotIndex >= m_Slots.size()) {
            return;
        }
        auto &slot = m_Slots[slotIndex];
        if (!slot.inUse) {
            return;
        }
        slot.inUse = false;
        slot.generation += 1;
        UpdateIndex(slot.record, false);
        slot.record = PatternRecord();
        m_FreeSlots.push_back(slotIndex);
    }

    void RegExpModule::Reset() {
        m_Slots.clear();
        m_FreeSlots.clear();
        m_Index.clear();
        m_CurrentFrame = 0;
        m_Metrics.activePatterns = 0;
        m_Metrics.hotPatterns = 0;
        m_Metrics.lastFrameTouched = 0;
    }

    void RegExpModule::Touch(PatternRecord &record) noexcept {
        record.lastTouchFrame = m_CurrentFrame;
        if (!record.hot) {
            record.hot = true;
            m_Metrics.hotPatterns += 1;
        }
        m_Metrics.lastFrameTouched = m_CurrentFrame;
    }

    void RegExpModule::RecomputeHotMetrics() noexcept {
        std::uint64_t hotCount = 0;
        for (auto &slot : m_Slots) {
            if (!slot.inUse) {
                continue;
            }
            auto &record = slot.record;
            if (m_CurrentFrame >= record.lastTouchFrame &&
                (m_CurrentFrame - record.lastTouchFrame) <= kHotFrameWindow) {
                record.hot = true;
                hotCount += 1;
            } else {
                record.hot = false;
            }
        }
        m_Metrics.hotPatterns = hotCount;
    }

    void RegExpModule::UpdateIndex(const PatternRecord &record, bool insert) {
        std::string key;
        key.reserve(record.pattern.size() + record.flags.size() + 1);
        key.append(record.pattern);
        key.push_back('\n');
        key.append(record.flags);
        if (insert) {
            m_Index[key] = record.handle;
        } else {
            auto it = m_Index.find(key);
            if (it != m_Index.end() && it->second == record.handle) {
                m_Index.erase(it);
            }
        }
    }
}


