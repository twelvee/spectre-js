#include "spectre/es2025/modules/math_module.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include "spectre/runtime.h"

namespace spectre::es2025 {
    namespace {
        constexpr std::string_view kName = "Math";
        constexpr std::string_view kSummary = "Math library intrinsics and numeric helper algorithms.";
        constexpr std::string_view kReference = "ECMA-262 Section 20.3";
        constexpr double kPi = 3.14159265358979323846264338327950288;
    }

    MathModule::Metrics::Metrics() noexcept
        : fastSinCalls(0),
          fastCosCalls(0),
          fastTanCalls(0),
          fastSqrtCalls(0),
          fastInvSqrtCalls(0),
          batchedFmaOps(0),
          reducedAngles(0),
          hornerEvaluations(0),
          dotProducts(0),
          lastFrameTouched(0),
          tableSize(0),
          gpuOptimized(false) {
    }

    MathModule::MathModule()
        : m_Runtime(nullptr),
          m_Subsystems(nullptr),
          m_Config{},
          m_GpuEnabled(false),
          m_Initialized(false),
          m_CurrentFrame(0),
          m_Table(),
          m_Metrics() {
    }

    std::string_view MathModule::Name() const noexcept {
        return kName;
    }

    std::string_view MathModule::Summary() const noexcept {
        return kSummary;
    }

    std::string_view MathModule::SpecificationReference() const noexcept {
        return kReference;
    }

    void MathModule::Initialize(const ModuleInitContext &context) {
        m_Runtime = &context.runtime;
        m_Subsystems = &context.subsystems;
        m_Config = context.config;
        m_GpuEnabled = context.config.enableGpuAcceleration;
        m_CurrentFrame = 0;
        m_Table.resize(kTableResolution + 1);
        for (std::size_t i = 0; i < m_Table.size(); ++i) {
            double angle = (static_cast<double>(i) / static_cast<double>(kTableResolution)) * kTwoPi;
            m_Table[i].sinValue = static_cast<float>(std::sin(angle));
            m_Table[i].cosValue = static_cast<float>(std::cos(angle));
        }
        // Ensure wrap-around samples are identical for interpolation without branches.
        m_Table.back() = m_Table.front();
        m_Metrics = Metrics();
        m_Metrics.gpuOptimized = m_GpuEnabled;
        m_Metrics.tableSize = m_Table.size();
        m_Initialized = true;
    }

    void MathModule::Tick(const TickInfo &info, const ModuleTickContext &) noexcept {
        m_CurrentFrame = info.frameIndex;
        m_Metrics.lastFrameTouched = m_CurrentFrame;
    }

    void MathModule::OptimizeGpu(const ModuleGpuContext &context) noexcept {
        m_GpuEnabled = context.enableAcceleration;
        m_Metrics.gpuOptimized = m_GpuEnabled;
    }

    void MathModule::Reconfigure(const RuntimeConfig &config) {
        m_Config = config;
        m_GpuEnabled = config.enableGpuAcceleration;
        m_Metrics.gpuOptimized = m_GpuEnabled;
    }

    double MathModule::FastSin(double radians) const noexcept {
        m_Metrics.fastSinCalls += 1;
        if (!std::isfinite(radians) || m_Table.size() < 2) {
            return std::sin(radians);
        }
        return TableLerp(radians, false);
    }

    double MathModule::FastCos(double radians) const noexcept {
        m_Metrics.fastCosCalls += 1;
        if (!std::isfinite(radians) || m_Table.size() < 2) {
            return std::cos(radians);
        }
        return TableLerp(radians, true);
    }

    double MathModule::FastTan(double radians) const noexcept {
        auto sinValue = FastSin(radians);
        auto cosValue = FastCos(radians);
        m_Metrics.fastTanCalls += 1;
        if (!std::isfinite(sinValue) || !std::isfinite(cosValue)) {
            return std::tan(radians);
        }
        double epsilon = 1e-12;
        if (std::fabs(cosValue) < epsilon) {
            return sinValue >= 0.0
                       ? std::numeric_limits<double>::infinity()
                       : -std::numeric_limits<double>::infinity();
        }
        return sinValue / cosValue;
    }

    double MathModule::FastSqrt(double value) const noexcept {
        m_Metrics.fastSqrtCalls += 1;
        if (value < 0.0) {
            return std::numeric_limits<double>::quiet_NaN();
        }
        return std::sqrt(value);
    }

    float MathModule::FastInverseSqrt(float value) const noexcept {
        m_Metrics.fastInvSqrtCalls += 1;
        if (value == 0.0f) {
            return std::numeric_limits<float>::infinity();
        }
        if (value < 0.0f) {
            return std::numeric_limits<float>::quiet_NaN();
        }
        float x = value;
        std::uint32_t i = 0;
        std::memcpy(&i, &x, sizeof(float));
        i = 0x5f3759dfu - (i >> 1);
        std::memcpy(&x, &i, sizeof(float));
        x = x * (1.5f - 0.5f * value * x * x);
        return x;
    }

    double MathModule::ReduceAngle(double radians) const noexcept {
        m_Metrics.reducedAngles += 1;
        if (!std::isfinite(radians)) {
            return radians;
        }
        double normalized = Normalize(radians);
        if (normalized > kPi) {
            normalized -= kTwoPi;
        }
        return normalized;
    }

    double MathModule::Lerp(double a, double b, double t) const noexcept {
        auto clampedT = Clamp(t, 0.0, 1.0);
        return a + (b - a) * clampedT;
    }

    double MathModule::Clamp(double value, double minValue, double maxValue) const noexcept {
        if (minValue > maxValue) {
            std::swap(minValue, maxValue);
        }
        if (value < minValue) {
            return minValue;
        }
        if (value > maxValue) {
            return maxValue;
        }
        return value;
    }

    double MathModule::Dot3(const double *a, const double *b) const noexcept {
        if (!a || !b) {
            return 0.0;
        }
        m_Metrics.dotProducts += 1;
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    }

    double MathModule::Dot4(const double *a, const double *b) const noexcept {
        if (!a || !b) {
            return 0.0;
        }
        m_Metrics.dotProducts += 1;
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    }

    void MathModule::BatchedFma(const double *a,
                                const double *b,
                                const double *c,
                                double *out,
                                std::size_t count) const noexcept {
        if (!a || !b || !c || !out) {
            return;
        }
        for (std::size_t i = 0; i < count; ++i) {
            out[i] = a[i] * b[i] + c[i];
        }
        m_Metrics.batchedFmaOps += count;
    }

    double MathModule::Horner(const double *coefficients, std::size_t count, double x) const noexcept {
        if (!coefficients || count == 0) {
            return 0.0;
        }
        double result = coefficients[0];
        for (std::size_t i = 1; i < count; ++i) {
            result = result * x + coefficients[i];
        }
        m_Metrics.hornerEvaluations += 1;
        return result;
    }

    const MathModule::Metrics &MathModule::GetMetrics() const noexcept {
        return m_Metrics;
    }

    bool MathModule::GpuEnabled() const noexcept {
        return m_GpuEnabled;
    }

    std::size_t MathModule::TableIndex(double radians) const noexcept {
        if (m_Table.size() < 2) {
            return 0;
        }
        auto resolution = m_Table.size() - 1;
        double position = Normalize(radians) * (static_cast<double>(resolution) / kTwoPi);
        auto index = static_cast<std::size_t>(position);
        if (index >= resolution) {
            index = resolution - 1;
        }
        return index;
    }

    double MathModule::TableLerp(double radians, bool cosine) const noexcept {
        if (m_Table.size() < 2) {
            return cosine ? std::cos(radians) : std::sin(radians);
        }
        auto resolution = m_Table.size() - 1;
        double position = Normalize(radians) * (static_cast<double>(resolution) / kTwoPi);
        auto index = static_cast<std::size_t>(position);
        double fraction = position - static_cast<double>(index);
        if (index >= resolution) {
            index = resolution - 1;
            fraction = 1.0;
        }
        const auto &current = m_Table[index];
        const auto &next = m_Table[index + 1];
        double a = cosine ? static_cast<double>(current.cosValue) : static_cast<double>(current.sinValue);
        double b = cosine ? static_cast<double>(next.cosValue) : static_cast<double>(next.sinValue);
        return a + (b - a) * fraction;
    }

    double MathModule::Normalize(double radians) const noexcept {
        if (!std::isfinite(radians)) {
            return 0.0;
        }
        double revolutions = std::floor(radians / kTwoPi);
        double normalized = radians - revolutions * kTwoPi;
        if (normalized < 0.0) {
            normalized += kTwoPi;
        }
        return normalized;
    }
}