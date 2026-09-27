#pragma once

#include <cstdint>
#include <functional>

namespace SupportReportLauncher {
    enum class Status {
        Success,
        AlreadyRunning,
        LauncherMissing,
        StartFailed,
        GenerationFailed
    };

    struct Result {
        Status status;
        std::uint32_t errorCode = 0;
    };

    using CompletionCallback = std::function<void(Result)>;

    // Runs the installed DwemerDistro diagnostic command on the CHIM worker pool.
    void GenerateAsync(CompletionCallback callback);
}
