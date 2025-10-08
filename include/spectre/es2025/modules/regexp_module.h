
#pragma once

#include <cstdint>
#include <limits>
#include <regex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "spectre/config.h"
#include "spectre/status.h"
#include "spectre/es2025/module.h"

namespace spectre::es2025 {
    class RegExpModule final : public Module {
    public:
        using Handle = std::uint64_t;

        enum class Flag : std::uint8_t {
            Global = 1 << 0,
            IgnoreCase = 1 << 1,
            Multiline = 1 << 2,
            DotAll = 1 << 3,
            Unicode = 1 << 4,
            Sticky = 1 << 5
        };

        struct Metrics {
            std::uint64_t compiled;
            std::uint64_t cacheHits;
            std::uint64_t cacheMisses;
            std::uint64_t executions;
            std::uint64_t matches;
            std::uint64_t mismatches;
            std::uint64_t replacements;
            std::uint64_t splits;
            std::uint64_t resets;
            std::uint64_t activePatterns;
            std::uint64_t hotPatterns;
            std::uint64_t lastFrameTouched;
            bool gpuOptimized;

            Metrics() noexcept;
        };
        struct MatchRange {
            std::uint32_t begin;
            std::uint32_t end;

            MatchRange() noexcept;
            MatchRange(std::uint32_t b, std::uint32_t e) noexcept;
        };

        struct MatchResult {
            bool matched;
            std::uint32_t index;
            std::uint32_t length;
            std::uint32_t nextIndex;
            std::vector<MatchRange> groups;

            MatchResult() noexcept;
        };

        RegExpModule();

        std::string_view Name() const noexcept override;
        std::string_view Summary() const noexcept override;
        std::string_view SpecificationReference() const noexcept override;

        void Initialize(const ModuleInitContext &context) override;
        void Tick(const TickInfo &info, const ModuleTickContext &context) noexcept override;
        void OptimizeGpu(const ModuleGpuContext &context) noexcept override;
        void Reconfigure(const RuntimeConfig &config) override;

        StatusCode Compile(std::string_view pattern, std::string_view flags, Handle &outHandle);
        StatusCode Destroy(Handle handle);
        StatusCode Exec(Handle handle,
                        std::string_view input,
                        std::size_t startIndex,
                        MatchResult &outResult);
        StatusCode Exec(Handle handle,
                        std::string_view input,
                        MatchResult &outResult);
        StatusCode Test(Handle handle, std::string_view input, bool &outMatched);
        StatusCode Replace(Handle handle,
                           std::string_view input,
                           std::string_view replacement,
                           std::string &outOutput,
                           bool allOccurrences = true);
        StatusCode Split(Handle handle,
                         std::string_view input,
                         std::size_t limit,
                         std::vector<std::string> &outParts);
        StatusCode ResetLastIndex(Handle handle);
        std::size_t LastIndex(Handle handle) const noexcept;
        StatusCode Source(Handle handle, std::string &outPattern, std::string &outFlags) const;

        const Metrics &GetMetrics() const noexcept;
        bool GpuEnabled() const noexcept;

    private:
        struct PatternRecord {
            PatternRecord() noexcept
                : handle(0),
                  slot(0),
                  generation(0),
                  pattern(),
                  flags(),
                  decorated(),
                  regex(),
                  syntax(std::regex_constants::ECMAScript),
                  global(false),
                  ignoreCase(false),
                  multiline(false),
                  dotAll(false),
                  unicode(false),
                  sticky(false),
                  lastIndex(0),
                  lastTouchFrame(0),
                  hot(false) {}

            Handle handle;
            std::uint32_t slot;
            std::uint32_t generation;
            std::string pattern;
            std::string flags;
            std::string decorated;
            std::regex regex;
            std::regex_constants::syntax_option_type syntax;
            bool global;
            bool ignoreCase;
            bool multiline;
            bool dotAll;
            bool unicode;
            bool sticky;
            std::size_t lastIndex;
            std::uint64_t lastTouchFrame;
            bool hot;
        };

        struct SlotRecord {
            SlotRecord() noexcept : inUse(false), generation(0), record() {}
            bool inUse;
            std::uint32_t generation;
            PatternRecord record;
        };

        struct ParsedFlags {
            bool global;
            bool ignoreCase;
            bool multiline;
            bool dotAll;
            bool unicode;
            bool sticky;
            std::string normalized;

            ParsedFlags() noexcept;
        };
        SpectreRuntime *m_Runtime;
        detail::SubsystemSuite *m_Subsystems;
        RuntimeConfig m_Config;
        bool m_GpuEnabled;
        bool m_Initialized;
        std::uint64_t m_CurrentFrame;
        Metrics m_Metrics;
        std::vector<SlotRecord> m_Slots;
        std::vector<std::uint32_t> m_FreeSlots;
        std::unordered_map<std::string, Handle> m_Index;

        static Handle EncodeHandle(std::uint32_t slot, std::uint32_t generation) noexcept;
        static std::uint32_t DecodeSlot(Handle handle) noexcept;
        static std::uint32_t DecodeGeneration(Handle handle) noexcept;

        PatternRecord *FindMutable(Handle handle) noexcept;
        const PatternRecord *Find(Handle handle) const noexcept;

        std::uint32_t AcquireSlot();
        void ReleaseSlot(std::uint32_t slotIndex);
        void Reset();
        void Touch(PatternRecord &record) noexcept;
        void RecomputeHotMetrics() noexcept;

        StatusCode ParseFlags(std::string_view flags, ParsedFlags &outFlags, std::string &outNormalized) const;
        StatusCode DecoratePattern(const ParsedFlags &flags, std::string_view pattern, std::string &outDecorated) const;
        void UpdateIndex(const PatternRecord &record, bool insert);
    };
}



