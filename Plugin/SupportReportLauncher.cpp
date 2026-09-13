#include "SupportReportLauncher.h"

#include "ThreadPool.h"

#include <windows.h>

#include <chrono>
#include <string>

namespace logger = SKSE::log;

namespace SupportReportLauncher {
    void GenerateAsync(CompletionCallback callback) {
        ThreadPool::getInstance().enqueue(
            "GenerateSupportReport",
            [callback = std::move(callback)]() {
                constexpr wchar_t kLauncherPath[] = L"C:\\DwemerDistro\\DwemerDistro.exe";
                if (GetFileAttributesW(kLauncherPath) == INVALID_FILE_ATTRIBUTES) {
                    logger::error("[Support Report] DwemerDistro launcher was not found at C:\\DwemerDistro\\DwemerDistro.exe");
                    callback({Status::LauncherMissing});
                    return;
                }

                std::wstring commandLine = std::wstring(L"\"") + kLauncherPath +
                                           L"\" --generate-diagnostics --open-output-folder";
                STARTUPINFOW startupInfo{};
                startupInfo.cb = sizeof(startupInfo);
                PROCESS_INFORMATION processInfo{};

                if (!CreateProcessW(
                        kLauncherPath,
                        commandLine.data(),
                        nullptr,
                        nullptr,
                        FALSE,
                        CREATE_NO_WINDOW,
                        nullptr,
                        nullptr,
                        &startupInfo,
                        &processInfo)) {
                    const DWORD error = ::GetLastError();
                    logger::error("[Support Report] Failed to start DwemerDistro diagnostics (Win32 error {})", error);
                    callback({Status::StartFailed, error});
                    return;
                }

                WaitForSingleObject(processInfo.hProcess, INFINITE);
                DWORD exitCode = 1;
                if (!GetExitCodeProcess(processInfo.hProcess, &exitCode)) {
                    exitCode = 1;
                }
                CloseHandle(processInfo.hThread);
                CloseHandle(processInfo.hProcess);

                logger::info("[Support Report] DwemerDistro diagnostics exited with code {}", exitCode);
                if (exitCode == 0) {
                    callback({Status::Success});
                } else if (exitCode == 3) {
                    callback({Status::AlreadyRunning});
                } else {
                    callback({Status::GenerationFailed, exitCode});
                }
            },
            "DwemerDistroDiagnostics",
            std::chrono::minutes(10));
    }
}
