#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "spectre/config.h"
#include "spectre/es2025/module.h"

namespace spectre::es2025 {
    class MathModule final : public Module {
    public:
        struct Metrics {
            std::uint64_t fastSinCalls;
            std::uint64_t fastCosCalls;
            std::uint64_t fastTanCalls;
            std::uint64_t fastSqrtCalls;
            std::uint64_t fastInvSqrtCalls;
            std::uint64_t batchedFmaOps;
            std::uint64_t reducedAngles;
            std::uint64_t hornerEvaluations;
            std::uint64_t dotProducts;
            std::uint64_t lastFrameTouched;
            std::uint64_t tableSize;
            bool gpuOptimized;

            Metrics() noexcept;
        };

        MathModule();

        std::string_view Name() const noexcept override;

        std::string_view Summary() const noexcept override;

        std::string_view SpecificationReference() const noexcept override;

        void Initialize(const ModuleInitContext &context) override;

        void Tick(const TickInfo &info, const ModuleTickContext &context) noexcept override;

        void OptimizeGpu(const ModuleGpuContext &context) noexcept override;

        void Reconfigure(const RuntimeConfig &config) override;

        double FastSin(double radians) const noexcept;

        double FastCos(double radians) const noexcept;

        double FastTan(double radians) const noexcept;

        double FastSqrt(double value) const noexcept;

        float FastInverseSqrt(float value) const noexcept;

        double ReduceAngle(double radians) const noexcept;

        double Lerp(double a, double b, double t) const noexcept;

        double Clamp(double value, double minValue, double maxValue) const noexcept;

        double Dot3(const double *a, const double *b) const noexcept;

        double Dot4(const double *a, const double *b) const noexcept;

        void BatchedFma(const double *a, const double *b, const double *c, double *out,
                        std::size_t count) const noexcept;

        double Horner(const double *coefficients, std::size_t count, double x) const noexcept;

        const Metrics &GetMetrics() const noexcept;

        bool GpuEnabled() const noexcept;

    private:
        static constexpr std::size_t kTableResolution = 2048;
        static constexpr double kTwoPi = 6.28318530717958647692528676655900577;
        static constexpr double kHalfPi = 1.57079632679489661923132169163975144;

        struct TableEntry {
            float sinValue;
            float cosValue;
        };

        SpectreRuntime *m_Runtime;
        detail::SubsystemSuite *m_Subsystems;
        RuntimeConfig m_Config;
        bool m_GpuEnabled;
        bool m_Initialized;
        std::uint64_t m_CurrentFrame;
        std::vector<TableEntry> m_Table;
        mutable Metrics m_Metrics;

        std::size_t TableIndex(double radians) const noexcept;

        double TableLerp(double radians, bool cosine) const noexcept;

        double Normalize(double radians) const noexcept;
    };
}