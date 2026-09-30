#include "SupportReportLauncher.h"

#include "ThreadPool.h"

#include <windows.h>

#include <chrono>
#include <filesystem>
#include <string>

namespace logger = SKSE::log;

namespace SupportReportLauncher {
    // Read only the registered executable; never execute the protocol's command or arguments.
    static std::wstring ResolveLauncherPath() {
        const auto isLauncher = [](const std::wstring& candidate) {
            const std::filesystem::path path(candidate);
            const DWORD attributes = GetFileAttributesW(candidate.c_str());
            return path.is_absolute() &&
                   _wcsicmp(path.filename().c_str(), L"DwemerDistro.exe") == 0 &&
                   attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
        };

        std::wstring command(32768, L'\0');
        DWORD bytes = static_cast<DWORD>(command.size() * sizeof(wchar_t));
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Classes\\dwemerdistro\\shell\\open\\command",
                         nullptr, RRF_RT_REG_SZ, nullptr, command.data(), &bytes) == ERROR_SUCCESS) {
            command.resize(wcsnlen(command.c_str(), command.size()));
            // The launcher registers a quoted absolute path, including for installs with spaces.
            if (!command.empty() && command.front() == L'"') {
                const auto end = command.find(L'"', 1);
                if (end != std::wstring::npos &&
                    (end + 1 == command.size() || command[end + 1] == L' ')) {
                    const auto candidate = command.substr(1, end - 1);
                    if (isLauncher(candidate)) {
                        logger::info("[Support Report] Using the registered DwemerDistro launcher location");
                        return candidate;
                    }
                }
            }
        }

        const std::wstring fallback = L"C:\\DwemerDistro\\DwemerDistro.exe";
        if (isLauncher(fallback)) {
            logger::info("[Support Report] Using the default DwemerDistro launcher location");
            return fallback;
        }
        return {};
    }

    void GenerateAsync(CompletionCallback callback) {
        ThreadPool::getInstance().enqueue(
            "GenerateSupportReport",
            [callback = std::move(callback)]() {
                const auto launcherPath = ResolveLauncherPath();
                if (launcherPath.empty()) {
                    logger::warn("[Support Report] DwemerDistro launcher was not found; skipping log generation only");
                    callback({Status::LauncherMissing});
                    return;
                }

                std::wstring commandLine = std::wstring(L"\"") + launcherPath +
                                           L"\" --generate-diagnostics --open-output-folder";
                STARTUPINFOW startupInfo{};
                startupInfo.cb = sizeof(startupInfo);
                PROCESS_INFORMATION processInfo{};

                if (!CreateProcessW(
                        launcherPath.c_str(),
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
