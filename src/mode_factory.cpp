#include "mode_adapter.h"

#include <memory>

namespace spectre::detail {
    std::unique_ptr<ModeAdapter> CreateSingleThreadAdapter(const RuntimeConfig &config);

    std::unique_ptr<ModeAdapter> CreateMultiThreadAdapter(const RuntimeConfig &config);

    std::unique_ptr<ModeAdapter> MakeModeAdapter(const RuntimeConfig &config) {
        if (config.mode == RuntimeMode::SingleThread) {
            return CreateSingleThreadAdapter(config);
        }
        if (config.mode == RuntimeMode::MultiThread) {
            return CreateMultiThreadAdapter(config);
        }
        return nullptr;
    }
}