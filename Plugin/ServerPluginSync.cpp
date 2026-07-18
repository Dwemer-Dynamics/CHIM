#include "ServerPluginSync.h"

#include "Conf.h"
#include "ThreadPool.h"
#include "json.hpp"

#include <Windows.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace logger = SKSE::log;
using json = nlohmann::json;

namespace
{
    constexpr std::uintmax_t kMaximumPackageBytes = 512ULL * 1024ULL * 1024ULL;
    constexpr std::size_t kUploadChunkBytes = 1024ULL * 1024ULL;
    std::atomic_bool g_syncScheduled{false};

    struct ServerPluginPackage
    {
        std::string name;
        std::string version;
        std::filesystem::path archive;
        std::uintmax_t size{0};
    };

    struct HttpResponse
    {
        DWORD status{0};
        std::string body;

        bool ok() const { return status >= 200 && status < 300; }
    };

    std::wstring ToWide(const std::string& value)
    {
        if (value.empty()) return {};
        const int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
        if (size <= 1) return std::wstring(value.begin(), value.end());
        std::wstring result(static_cast<std::size_t>(size), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, result.data(), size);
        result.resize(static_cast<std::size_t>(size - 1));
        return result;
    }

    bool IsSafePluginName(const std::string& value)
    {
        static const std::regex pattern(R"(^[A-Za-z0-9][A-Za-z0-9 ._-]{0,63}$)");
        return std::regex_match(value, pattern) && value.back() != '.' && value.back() != ' ';
    }

    bool IsSafeVersion(const std::string& value)
    {
        static const std::regex pattern(R"(^[0-9A-Za-z][0-9A-Za-z._+-]{0,63}$)");
        return std::regex_match(value, pattern);
    }

    bool IsSupportedPackageArchive(const std::filesystem::path& path)
    {
        std::string extension = path.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
        return extension == ".dwpkg" || extension == ".zip";
    }

    std::string PackageApiPath(const std::string& action)
    {
        std::string path = Conf::getInstance().getPath();
        const auto position = path.find("comm.php");
        if (position == std::string::npos) {
            logger::error("[SERVER_PLUGIN_SYNC] Configured server path does not contain comm.php: {}", path);
            return {};
        }
        path.replace(position, 8, "ui/api/plugin_packages.php");
        path.append("?action=").append(action);
        return path;
    }

    HttpResponse Request(const std::string& method, const std::string& path, const std::string& contentType,
                         const char* data, std::size_t size)
    {
        HttpResponse response;
        if (path.empty() || size > static_cast<std::size_t>(std::numeric_limits<DWORD>::max())) return response;

        const std::string server = Conf::getInstance().getServer();
        const std::string portText = Conf::getInstance().getPort();
        INTERNET_PORT port = 0;
        try {
            const int parsed = std::stoi(portText);
            if (parsed < 1 || parsed > 65535) return response;
            port = static_cast<INTERNET_PORT>(parsed);
        } catch (...) {
            return response;
        }

        HINTERNET session = WinHttpOpen(L"CHIM Server Plugin Sync/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) return response;
        WinHttpSetTimeouts(session, 5000, 5000, 30000, 30000);

        HINTERNET connection = WinHttpConnect(session, ToWide(server).c_str(), port, 0);
        if (!connection) {
            WinHttpCloseHandle(session);
            return response;
        }
        HINTERNET request = WinHttpOpenRequest(connection, ToWide(method).c_str(), ToWide(path).c_str(), nullptr,
                                               WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
        if (!request) {
            WinHttpCloseHandle(connection);
            WinHttpCloseHandle(session);
            return response;
        }

        const std::wstring headers = ToWide("Content-Type: " + contentType + "\r\nAccept: application/json\r\n");
        const DWORD bodySize = static_cast<DWORD>(size);
        const BOOL sent = WinHttpSendRequest(
            request, headers.c_str(), static_cast<DWORD>(-1L), size > 0 ? const_cast<char*>(data) : WINHTTP_NO_REQUEST_DATA,
            bodySize, bodySize, 0);
        if (sent && WinHttpReceiveResponse(request, nullptr)) {
            DWORD statusSize = sizeof(response.status);
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &response.status, &statusSize, WINHTTP_NO_HEADER_INDEX);
            DWORD available = 0;
            while (WinHttpQueryDataAvailable(request, &available) && available > 0) {
                std::string buffer(available, '\0');
                DWORD read = 0;
                if (!WinHttpReadData(request, buffer.data(), available, &read) || read == 0) break;
                response.body.append(buffer.data(), read);
            }
        }
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return response;
    }

    HttpResponse PostJson(const std::string& action, const json& payload)
    {
        const std::string body = payload.dump();
        return Request("POST", PackageApiPath(action), "application/json", body.data(), body.size());
    }

    std::optional<json> ParseResponse(const HttpResponse& response, const std::string& operation)
    {
        if (!response.ok()) {
            logger::warn("[SERVER_PLUGIN_SYNC] {} failed with HTTP {}", operation, response.status);
            return std::nullopt;
        }
        json parsed = json::parse(response.body, nullptr, false);
        if (parsed.is_discarded() || !parsed.value("ok", false)) {
            const std::string error = parsed.is_object() ? parsed.value("error", "invalid server response") : "invalid server response";
            logger::warn("[SERVER_PLUGIN_SYNC] {} failed: {}", operation, error);
            return std::nullopt;
        }
        return parsed;
    }

    std::vector<ServerPluginPackage> FindPackages()
    {
        const std::filesystem::path root("Data/CHIM/server-plugins");
        std::vector<ServerPluginPackage> packages;
        std::error_code error;
        if (!std::filesystem::is_directory(root, error)) return packages;

        for (const auto& directory : std::filesystem::directory_iterator(root, error)) {
            if (error || !directory.is_directory()) continue;
            const std::string name = directory.path().filename().string();
            if (!IsSafePluginName(name)) {
                logger::warn("[SERVER_PLUGIN_SYNC] Ignoring unsafe plugin folder name: {}", name);
                continue;
            }

            std::vector<std::filesystem::directory_entry> archives;
            for (const auto& file : std::filesystem::directory_iterator(directory.path(), error)) {
                if (!error && file.is_regular_file() && IsSupportedPackageArchive(file.path())) archives.push_back(file);
            }
            if (archives.empty()) continue;
            std::sort(archives.begin(), archives.end(), [](const auto& left, const auto& right) {
                return left.last_write_time() > right.last_write_time();
            });
            if (archives.size() > 1) {
                logger::warn("[SERVER_PLUGIN_SYNC] {} has multiple packages; using newest file {}", name,
                             archives.front().path().filename().string());
            }
            const auto& archive = archives.front();
            const std::string version = archive.path().stem().string();
            if (!IsSafeVersion(version)) {
                logger::warn("[SERVER_PLUGIN_SYNC] Ignoring {} because package filename is not a valid version: {}", name,
                             archive.path().filename().string());
                continue;
            }
            const auto size = archive.file_size(error);
            if (error || size == 0 || size > kMaximumPackageBytes) {
                logger::warn("[SERVER_PLUGIN_SYNC] Ignoring {} {} because its size is invalid", name, version);
                continue;
            }
            packages.push_back({name, version, archive.path(), size});
        }
        std::sort(packages.begin(), packages.end(), [](const auto& left, const auto& right) {
            return left.name < right.name;
        });
        return packages;
    }

    bool UploadPackage(const ServerPluginPackage& package)
    {
        auto probe = ParseResponse(PostJson("probe", {{"name", package.name}, {"version", package.version}}),
                                   "probe " + package.name);
        if (!probe) return false;
        if (!(*probe)["package"].value("upload_required", true)) {
            logger::info("[SERVER_PLUGIN_SYNC] {} {} is already current", package.name, package.version);
            return true;
        }

        const std::size_t totalChunks = static_cast<std::size_t>((package.size + kUploadChunkBytes - 1) / kUploadChunkBytes);
        auto started = ParseResponse(PostJson("start-upload", {
            {"name", package.name},
            {"version", package.version},
            {"archive_name", package.archive.filename().string()},
            {"size", package.size},
            {"total_chunks", totalChunks},
        }), "start upload " + package.name);
        if (!started) return false;
        const std::string uploadId = (*started)["upload"].value("upload_id", "");
        if (uploadId.empty()) return false;

        std::ifstream stream(package.archive, std::ios::binary);
        if (!stream.is_open()) return false;
        std::vector<char> buffer(kUploadChunkBytes);
        for (std::size_t index = 0; index < totalChunks; ++index) {
            stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto count = stream.gcount();
            if (count <= 0) return false;
            const std::string action = "upload-chunk&upload_id=" + uploadId + "&index=" + std::to_string(index);
            const auto response = Request("POST", PackageApiPath(action), "application/octet-stream", buffer.data(),
                                          static_cast<std::size_t>(count));
            auto uploaded = ParseResponse(response, "upload chunk " + std::to_string(index + 1) + " for " + package.name);
            if (!uploaded) return false;
            if (index + 1 == totalChunks) {
                const auto& result = (*uploaded)["upload"];
                if (!result.value("complete", false) || result["job"].value("status", "") != "completed") {
                    logger::warn("[SERVER_PLUGIN_SYNC] {} {} was uploaded but activation failed: {}", package.name,
                                 package.version, result["job"].value("error", "unknown error"));
                    return false;
                }
            }
        }
        logger::info("[SERVER_PLUGIN_SYNC] Installed {} {}", package.name, package.version);
        return true;
    }

    bool SyncPackages()
    {
        const auto packages = FindPackages();
        if (packages.empty()) {
            logger::info("[SERVER_PLUGIN_SYNC] No bundled server plugins found");
            return true;
        }
        logger::info("[SERVER_PLUGIN_SYNC] Found {} bundled server plugin(s)", packages.size());
        bool complete = true;
        for (const auto& package : packages) {
            if (!UploadPackage(package)) complete = false;
        }
        return complete;
    }
}

void ScheduleServerPluginSync()
{
    bool expected = false;
    if (!g_syncScheduled.compare_exchange_strong(expected, true)) return;
    try {
        ThreadPool::getInstance().enqueue("ServerPluginSync", [] {
            if (SyncPackages()) return;
            logger::warn("[SERVER_PLUGIN_SYNC] Startup sync was incomplete; retrying once in 15 seconds");
            std::this_thread::sleep_for(std::chrono::seconds(15));
            if (!SyncPackages()) logger::warn("[SERVER_PLUGIN_SYNC] Automatic server plugin sync remains incomplete");
        }, "startup", std::chrono::milliseconds(0));
    } catch (const std::exception& error) {
        g_syncScheduled = false;
        logger::warn("[SERVER_PLUGIN_SYNC] Could not schedule automatic sync: {}", error.what());
    }
}
