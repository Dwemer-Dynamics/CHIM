#include <algorithm>
#include <memory>
#include <vector>
#include "HTTPUploader.h"
#include "Conf.h"


#pragma comment(lib, "Wininet.lib")
#pragma comment(lib, "winhttp.lib")

namespace logger = SKSE::log;

#define DEFAULT_USERAGENT L"Mozilla/5.0 (Windows NT 6.1; WOW64; rv:40.0) Gecko/20100101 Firefox/40.1"
#define BUFSIZE 1024


extern const wchar_t *StringToWideString(std::string &str);
extern int MutexGetScreenShotSendMode();
extern void MutexSetScreenShotSendMode(int newVal);

struct FileCloser {
    typedef HANDLE pointer;

    void operator()(HANDLE h) {
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
};

struct InetCloser {
    typedef HINTERNET pointer;

    void operator()(HINTERNET h) {
        if (h != NULL) InternetCloseHandle(h);
    }
};

bool HTTPUploader::WriteToInternet(HINTERNET hInet, const void *Data, DWORD DataSize) {
    const BYTE *pData = (const BYTE *)Data;
    DWORD dwBytes;

    while (DataSize > 0) {
        if (!InternetWriteFile(hInet, pData, DataSize, &dwBytes)) return false;
        pData += dwBytes;
        DataSize -= dwBytes;
    }

    return true;
}


std::string HTTPUploader::UploadFile(std::string data) {
    const char *szHeaders = "Content-Type: multipart/form-data; boundary=----974767299852498929531610575";
    const char *szContent =
        "------974767299852498929531610575\r\nContent-Disposition: form-data; name=\"file\"; "
        "filename=\"wavData.wav\"\r\nContent-Type: audio/x-wav\r\n\r\n";
    const char *szEndData = "\r\n------974767299852498929531610575--\r\n";

    std::string server = Conf::getInstance().getServer();
    std::string port = Conf::getInstance().getPort();
    std::string path = Conf::getInstance().getPath();
    auto pos = path.find("comm.php");
    if (pos != std::string::npos) {
        path.replace(pos, 8, "stt.php?stuff");
    }

    logger::info("Using STT: {}:{}{}", server, port, path);

    // Convert strings to wide strings for WinHTTP
    std::wstring wideServer(server.begin(), server.end());
    std::wstring widePath(path.begin(), path.end());
    std::wstring wideHeaders(szHeaders, szHeaders + strlen(szHeaders));

    // Open WinHTTP session
    HINTERNET hSession = WinHttpOpen(L"HTTPUploader", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, NULL, NULL, 0);
    if (!hSession) {
        logger::debug("WinHttpOpen failed");
        return "";
    }

    // Establish connection
    HINTERNET hConnect = WinHttpConnect(hSession, wideServer.c_str(), std::stoi(port), 0);
    if (!hConnect) {
        logger::debug("WinHttpConnect failed");
        WinHttpCloseHandle(hSession);
        return "";
    }

    // Open request
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", widePath.c_str(), NULL, WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!hRequest) {
        logger::debug("WinHttpOpenRequest failed");
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    // Add headers
    if (!WinHttpAddRequestHeaders(hRequest, wideHeaders.c_str(), -1, WINHTTP_ADDREQ_FLAG_ADD)) {
        logger::debug("WinHttpAddRequestHeaders failed");
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    // Prepare request body
    std::vector<char> requestBody;
    requestBody.insert(requestBody.end(), szContent, szContent + strlen(szContent));
    requestBody.insert(requestBody.end(), data.begin(), data.end());
    requestBody.insert(requestBody.end(), szEndData, szEndData + strlen(szEndData));

    // Send request
    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, requestBody.data(), requestBody.size(),
                            requestBody.size(), 0)) {
        logger::debug("WinHttpSendRequest failed");
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    // Wait for response
    if (!WinHttpReceiveResponse(hRequest, NULL)) {
        logger::debug("WinHttpReceiveResponse failed");
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    // Read response
    const int bufferSize = 4096;
    char buffer[bufferSize];
    DWORD bytesRead = 0;
    std::string response;

    do {
        if (!WinHttpReadData(hRequest, buffer, bufferSize, &bytesRead)) {
            logger::debug("WinHttpReadData failed");
            break;
        }
        if (bytesRead > 0) {
            response.append(buffer, bytesRead);
        }
    } while (bytesRead > 0);

    logger::debug("Raw response: {}", response);

    if (response.starts_with("<")) {
        response = "...";
        logger::info("Error using http://{}:{}{}", server, port, path);
    }

    // Clean up
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return response;
}

std::string HTTPUploader::UploadVoiceSample(std::string data, std::string codename, std::string originalName) {
    

    return UploadVoiceSampleWithText(data, codename, originalName, "");
}

std::string HTTPUploader::UploadVoiceSampleWithText(std::string data, std::string codename, std::string originalName,
                                                    std::string referenceText) {
    constexpr int kVoiceUploadTimeoutMs = 30000;
    const char *szHeaders = "Content-Type: multipart/form-data; boundary=----974767299852498929531610575";
    const char *szContent =
        "------974767299852498929531610575\r\nContent-Disposition: form-data; name=\"file\"; "
        "filename=\"wavData.wav\"\r\nContent-Type: audio/x-wav\r\n\r\n";
    const char *szEndData = "\r\n------974767299852498929531610575--\r\n";

    std::string server(Conf::getInstance().getServer());
    std::wstring wideServer = StringToWideString(server);
    std::string port = Conf::getInstance().getPort();
    std::wstring widePort = StringToWideString(port);

    std::string path = Conf::getInstance().getPath();
    auto pos = path.find("comm.php");
    if (pos != std::string::npos) {
        path.replace(pos, 8, "vsx.php?stuff");
    }
    path.append("&codename=").append(codename).append("&oname=").append(originalName);
    std::wstring widePath = StringToWideString(path);

    logger::info("Using VSX: {}", server + ":" + port + "/" + path);

    // Open a session
    HINTERNET hSession = WinHttpOpen(L"WinHTTP Voice Uploader/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, NULL, NULL, 0);
    if (!hSession) {
        logger::info("Failed to open WinHTTP session: {}", GetLastError());
        return "";
    }
    if (!WinHttpSetTimeouts(hSession, kVoiceUploadTimeoutMs, kVoiceUploadTimeoutMs, kVoiceUploadTimeoutMs,
                            kVoiceUploadTimeoutMs)) {
        logger::warn("Failed to set voice upload WinHTTP session timeouts ({} ms): {}",
                     kVoiceUploadTimeoutMs, GetLastError());
    }

    // Connect to the server
    HINTERNET hConnect = WinHttpConnect(hSession, wideServer.c_str(), std::stoi(port), 0);
    if (!hConnect) {
        logger::info("Failed to connect to server: {}", GetLastError());
        WinHttpCloseHandle(hSession);
        return "";
    }

    // Open the request
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", widePath.c_str(), NULL, WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!hRequest) {
        logger::info("Failed to open HTTP request: {}", GetLastError());
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }
    if (!WinHttpSetTimeouts(hRequest, kVoiceUploadTimeoutMs, kVoiceUploadTimeoutMs, kVoiceUploadTimeoutMs,
                            kVoiceUploadTimeoutMs)) {
        logger::warn("Failed to set voice upload WinHTTP request timeouts ({} ms): {}",
                     kVoiceUploadTimeoutMs, GetLastError());
    }

    // Add headers

    std::wstring wideHeaders = std::wstring(szHeaders, szHeaders + strlen(szHeaders));

    if (!WinHttpAddRequestHeaders(hRequest, wideHeaders.c_str(), -1, WINHTTP_ADDREQ_FLAG_ADD)) {
        logger::info("Failed to add headers: {}", GetLastError());
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    // Calculate the total size of the data
    size_t totalSize = strlen(szContent) + data.length() + strlen(szEndData);

    // Send the request
    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0,
                            static_cast<DWORD>(totalSize), 0)) {
        logger::info("Failed to send request: {}", GetLastError());
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    // Write the multipart form data
    DWORD bytesWritten = 0;
    if (!WinHttpWriteData(hRequest, szContent, static_cast<DWORD>(strlen(szContent)), &bytesWritten) ||
        bytesWritten != strlen(szContent)) {
        logger::info("Failed to write initial part of the data: {}", GetLastError());
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    bytesWritten = 0;
    if (!WinHttpWriteData(hRequest, data.c_str(), static_cast<DWORD>(data.length()), &bytesWritten) ||
        bytesWritten != data.length()) {
        logger::info("Failed to write main data: {}", GetLastError());
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    bytesWritten = 0;
    if (!WinHttpWriteData(hRequest, szEndData, static_cast<DWORD>(strlen(szEndData)), &bytesWritten) ||
        bytesWritten != strlen(szEndData)) {
        logger::info("Failed to write final part of the data: {}", GetLastError());
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    // Complete the request
    if (!WinHttpReceiveResponse(hRequest, NULL)) {
        logger::info("Failed to receive response: {}", GetLastError());
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    // Read the response
    std::string response;
    DWORD bytesRead = 0;
    const int bufferSize = 4096;
    char buffer[bufferSize];

    while (WinHttpReadData(hRequest, buffer, bufferSize, &bytesRead) && bytesRead > 0) {
        response.append(buffer, bytesRead);
    }

    if (response.starts_with("<")) {
        response.assign("...");
        logger::info("Error using http://{}", server + ":" + port + "/" + path);
    }

    // Clean up
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return response;
}

std::string HTTPUploader::UploadBookContent(std::string data, std::string title) {
    const char *szHeaders = "Content-Type: multipart/form-data; boundary=----974767299852498929531610575";
    const char *szContent =
        "------974767299852498929531610575\r\nContent-Disposition: form-data; name=\"file\"; "
        "filename=\"book_content.txt\"\r\nContent-Type: plain/text\r\n\r\n";
    const char *szEndData = "\r\n------974767299852498929531610575--\r\n";

    std::string server(Conf::getInstance().getServer());
    std::wstring wideServer = StringToWideString(server);
    std::string port = Conf::getInstance().getPort();
    std::wstring widePort = StringToWideString(port);

    std::string path(Conf::getInstance().getPath());
    auto pos = path.find("comm.php");
    if (pos != std::string::npos) {
        path.replace(pos, 8, "book.php?title=");
    }
    if (!title.empty()) {
        path.append(title);
    }

    path.append("&ts=");
    path.append(getCurrentTimeMillis());
    path.append("&gamets=");
    path.append(std::to_string(GetGameTimeStamp()));
    
    logger::info("Using BOOK: {}", server + ":" + port + "/" + path);

    std::wstring widePath = StringToWideString(path);

    // Open a session
    HINTERNET hSession = WinHttpOpen(L"WinHTTP Book Uploader/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, NULL, NULL, 0);
    if (!hSession) {
        logger::info("Failed to open WinHTTP session: {}", GetLastError());
        return "";
    }

    // Connect to the server
    HINTERNET hConnect = WinHttpConnect(hSession, wideServer.c_str(), std::stoi(port), 0);
    if (!hConnect) {
        logger::info("Failed to connect to server: {}", GetLastError());
        WinHttpCloseHandle(hSession);
        return "";
    }

    // Open the request
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", widePath.c_str(), NULL, WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!hRequest) {
        logger::info("Failed to open HTTP request: {}", GetLastError());
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    // Add headers

    std::wstring wideHeaders = std::wstring(szHeaders, szHeaders + strlen(szHeaders));

    if (!WinHttpAddRequestHeaders(hRequest, wideHeaders.c_str(), -1, WINHTTP_ADDREQ_FLAG_ADD)) {
        logger::info("Failed to add headers: {}", GetLastError());
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    // Calculate the total size of the data
    size_t totalSize = strlen(szContent) + data.length() + strlen(szEndData);

    // Send the request
    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0,
                            static_cast<DWORD>(totalSize), 0)) {
        logger::info("Failed to send request: {}", GetLastError());
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    // Write the multipart form data
    DWORD bytesWritten = 0;
    if (!WinHttpWriteData(hRequest, szContent, static_cast<DWORD>(strlen(szContent)), &bytesWritten) ||
        bytesWritten != strlen(szContent)) {
        logger::info("Failed to write initial part of the data: {}", GetLastError());
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    bytesWritten = 0;
    if (!WinHttpWriteData(hRequest, data.c_str(), static_cast<DWORD>(data.length()), &bytesWritten) ||
        bytesWritten != data.length()) {
        logger::info("Failed to write main data: {}", GetLastError());
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    bytesWritten = 0;
    if (!WinHttpWriteData(hRequest, szEndData, static_cast<DWORD>(strlen(szEndData)), &bytesWritten) ||
        bytesWritten != strlen(szEndData)) {
        logger::info("Failed to write final part of the data: {}", GetLastError());
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    // Complete the request
    if (!WinHttpReceiveResponse(hRequest, NULL)) {
        logger::info("Failed to receive response: {}", GetLastError());
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    // Read the response
    std::string response;
    DWORD bytesRead = 0;
    const int bufferSize = 4096;
    char buffer[bufferSize];

    while (WinHttpReadData(hRequest, buffer, bufferSize, &bytesRead) && bytesRead > 0) {
        response.append(buffer, bytesRead);
    }

    if (response.starts_with("<")) {
        response.assign("...");
        logger::info("Error using http://{}", server + ":" + port + "/" + path);
    }

    // Clean up
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return response;
}

std::string HTTPUploader::UploadCSVFile(std::string data, std::string filename, std::string fileType) {
    // Validate input parameters
    if (data.empty()) {
        logger::error("CSV upload failed: Empty data provided");
        return "";
    }
    
    if (filename.empty()) {
        logger::error("CSV upload failed: Empty filename provided");
        return "";
    }
    
    // Validate import type
    static const std::vector<std::string> validTypes = {
        "biography_import", "oghma_import", "dynamic_oghma_import", "description_import", "custom_action_import"
    };
    if (std::find(validTypes.begin(), validTypes.end(), fileType) == validTypes.end()) {
        logger::error("CSV upload failed: Invalid import type '{}'", fileType);
        return "";
    }
    
    // Validate file extension
    if (!filename.ends_with(".csv")) {
        logger::error("CSV upload failed: Invalid file extension for '{}'", filename);
        return "";
    }
    
    // Validate data size (10MB limit)
    const size_t MAX_FILE_SIZE = 10 * 1024 * 1024; // 10MB
    if (data.size() > MAX_FILE_SIZE) {
        logger::error("CSV upload failed: File '{}' is too large ({} bytes). Maximum allowed is 10MB.", 
                     filename, data.size());
        return "";
    }

    const char *szHeaders = "Content-Type: multipart/form-data; boundary=----974767299852498929531610575";
    const char *szContent =
        "------974767299852498929531610575\r\nContent-Disposition: form-data; name=\"file\"; "
        "filename=\"%s\"\r\nContent-Type: text/csv\r\n\r\n";
    const char *szEndData = "\r\n------974767299852498929531610575--\r\n";

    std::string server(Conf::getInstance().getServer());
    std::wstring wideServer = StringToWideString(server);
    std::string port = Conf::getInstance().getPort();

    std::string path(Conf::getInstance().getPath());
    auto pos = path.find("comm.php");
    if (pos != std::string::npos) {
        path.replace(pos, 8, "csv_import.php");
    }
    
    // Add parameters for file type and timestamps
    path.append("?type=").append(fileType);
    path.append("&filename=").append(filename);
    path.append("&ts=").append(getCurrentTimeMillis());
    path.append("&gamets=").append(std::to_string(GetGameTimeStamp()));
    
    logger::info("Using CSV Import: {}", server + ":" + port + "/" + path);

    std::wstring widePath = StringToWideString(path);

    // Open a session
    HINTERNET hSession = WinHttpOpen(L"WinHTTP CSV Uploader/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, NULL, NULL, 0);
    if (!hSession) {
        logger::error("Failed to open WinHTTP session: {}", GetLastError());
        return "";
    }

    // Connect to the server
    HINTERNET hConnect = WinHttpConnect(hSession, wideServer.c_str(), std::stoi(port), 0);
    if (!hConnect) {
        logger::error("Failed to connect to server: {}", GetLastError());
        WinHttpCloseHandle(hSession);
        return "";
    }

    // Open the request
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", widePath.c_str(), NULL, WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!hRequest) {
        logger::error("Failed to open HTTP request: {}", GetLastError());
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    // Prepare content header with filename
    char contentBuffer[512];
    snprintf(contentBuffer, sizeof(contentBuffer), szContent, filename.c_str());
    std::string formattedContent(contentBuffer);

    // Add headers
    std::wstring wideHeaders = std::wstring(szHeaders, szHeaders + strlen(szHeaders));

    if (!WinHttpAddRequestHeaders(hRequest, wideHeaders.c_str(), -1, WINHTTP_ADDREQ_FLAG_ADD)) {
        logger::error("Failed to add headers: {}", GetLastError());
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    // Calculate the total size of the data
    size_t totalSize = formattedContent.length() + data.length() + strlen(szEndData);

    // Send the request
    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0,
                            static_cast<DWORD>(totalSize), 0)) {
        logger::error("Failed to send request: {}", GetLastError());
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    // Write the multipart form data
    DWORD bytesWritten = 0;
    if (!WinHttpWriteData(hRequest, formattedContent.c_str(), static_cast<DWORD>(formattedContent.length()), &bytesWritten) ||
        bytesWritten != formattedContent.length()) {
        logger::error("Failed to write initial part of the data: {}", GetLastError());
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    bytesWritten = 0;
    if (!WinHttpWriteData(hRequest, data.c_str(), static_cast<DWORD>(data.length()), &bytesWritten) ||
        bytesWritten != data.length()) {
        logger::error("Failed to write main data: {}", GetLastError());
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    bytesWritten = 0;
    if (!WinHttpWriteData(hRequest, szEndData, static_cast<DWORD>(strlen(szEndData)), &bytesWritten) ||
        bytesWritten != strlen(szEndData)) {
        logger::error("Failed to write final part of the data: {}", GetLastError());
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    // Complete the request
    if (!WinHttpReceiveResponse(hRequest, NULL)) {
        logger::error("Failed to receive response: {}", GetLastError());
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    // Check HTTP status code
    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                           NULL, &statusCode, &statusSize, NULL)) {
        logger::info("HTTP Status Code: {}", statusCode);
    }

    // Read the response
    std::string response;
    DWORD bytesRead = 0;
    const int bufferSize = 4096;
    char buffer[bufferSize];

    while (WinHttpReadData(hRequest, buffer, bufferSize, &bytesRead) && bytesRead > 0) {
        response.append(buffer, bytesRead);
    }

    // Enhanced response handling for JSON format
    logger::debug("Raw CSV upload response (length: {}): '{}'", response.length(), response);
    
    if (response.starts_with("<")) {
        response.assign("...");
        logger::error("Error response from CSV import endpoint (HTML error page received)");
    } else if (response.empty()) {
        logger::error("Empty response from CSV import endpoint");
        response = "...";
    } else {
        // Try to validate if it's JSON
        if (response.starts_with("{") || response.starts_with("[")) {
            logger::info("Received JSON response: {}", response);
        } else {
            logger::info("Received plain text response: {}", response);
        }
    }

    // Clean up
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return response;
}

std::string HTTPUploader::UploadImagePng(const char *data, int size, std::string hints) {
    const char *szHeaders = "Content-Type: multipart/form-data; boundary=----974767299852498929531610575";
    const char *szContent =
        "------974767299852498929531610575\r\nContent-Disposition: form-data; name=\"file\"; "
        "filename=\"bmpData.png\"\r\nContent-Type: image/png\r\n\r\n";
    const char *szEndData = "\r\n------974767299852498929531610575--\r\n";

    std::string server = Conf::getInstance().getServer();
    std::string port = Conf::getInstance().getPort();
    std::string path = Conf::getInstance().getPath();
    auto pos = path.find("comm.php");
    if (pos != std::string::npos) {
        if (MutexGetScreenShotSendMode()==0)
            path.replace(pos, 8, "itt.php?stuff");
        else if (MutexGetScreenShotSendMode() == 1)
            path.replace(pos, 8, "pic.php?stuff");
        else if (MutexGetScreenShotSendMode() == 2)
            path.replace(pos, 8, "upl.php?stuff");
        else if (MutexGetScreenShotSendMode() == 3)
            path.replace(pos, 8, "item_image.php?stuff");
    }

    path.append("&format=png&hints=" + hints);
    logger::info("Using ITT: {}:{}{}", server, port, path);

    // Convert server and path to wide strings
    std::wstring wideServer(server.begin(), server.end());
    std::wstring widePath(path.begin(), path.end());
    std::wstring wideHeaders(szHeaders, szHeaders + strlen(szHeaders));

    // Open WinHTTP session
    HINTERNET hSession = WinHttpOpen(L"HTTPUploader", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, NULL, NULL, 0);
    if (!hSession) {
        logger::debug("WinHttpOpen failed");
        return "";
    }

    // Establish connection
    HINTERNET hConnect = WinHttpConnect(hSession, wideServer.c_str(), std::stoi(port), 0);
    if (!hConnect) {
        logger::debug("WinHttpConnect failed");
        WinHttpCloseHandle(hSession);
        return "";
    }

    // Open request
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", widePath.c_str(), NULL, WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!hRequest) {
        logger::debug("WinHttpOpenRequest failed");
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    // Add headers
    if (!WinHttpAddRequestHeaders(hRequest, wideHeaders.c_str(), -1, WINHTTP_ADDREQ_FLAG_ADD)) {
        logger::debug("WinHttpAddRequestHeaders failed");
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    // Prepare request data
    std::vector<char> requestBody;
    requestBody.insert(requestBody.end(), szContent, szContent + strlen(szContent));
    requestBody.insert(requestBody.end(), data, data + size);
    requestBody.insert(requestBody.end(), szEndData, szEndData + strlen(szEndData));

    // Send request
    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, requestBody.data(), requestBody.size(),
                            requestBody.size(), 0)) {
        logger::debug("WinHttpSendRequest failed");
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    // Wait for response
    if (!WinHttpReceiveResponse(hRequest, NULL)) {
        logger::debug("WinHttpReceiveResponse failed");
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    // Read response
    const int bufferSize = 4096;
    char buffer[bufferSize];
    DWORD bytesRead = 0;
    std::string response;

    do {
        if (!WinHttpReadData(hRequest, buffer, bufferSize, &bytesRead)) {
            logger::debug("WinHttpReadData failed");
            break;
        }
        if (bytesRead > 0) {
            response.append(buffer, bytesRead);
        }
    } while (bytesRead > 0);

    logger::debug("Raw response: {}", response);

    if (response.starts_with("<")) {
        response = "...";
        logger::info("Error using http://{}:{}{}", server, port, path);
    }

    // Clean up
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return response;
}
/*
std::string HTTPUploader::UploadImagePng(const char *data, int size, std::string hints) {
    const char *szHeaders = "Content-Type: multipart/form-data; boundary=----974767299852498929531610575";
    const char *szContent =
        "------974767299852498929531610575\r\nContent-Disposition: form-data; name=\"file\"; "
        "filename=\"bmpData.png\"\r\nContent-Type: image/png\r\n\r\n";
    const char *szEndData = "\r\n------974767299852498929531610575--\r\n";

    std::string server(Conf::getInstance().getServer());
    std::string port = Conf::getInstance().getPort();
    auto path = Conf::getInstance().getPath();
    auto pos = path.find("comm.php");
    if (pos != std::string::npos) {
        path.replace(pos, 8, "itt.php?stuff");
    }

    path.append("&format=png&hints=" + hints);

    logger::info("Using ITT: {}", server + ":" + port + "/" + path);
    std::string fullPathFile = path;

    std::unique_ptr<HINTERNET, InetCloser> io(
        InternetOpen(DEFAULT_USERAGENT, INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0));
    if (io.get() == NULL) {
        return "";
    }

    std::unique_ptr<HINTERNET, InetCloser> ic(InternetConnect(io.get(), StringToWideString(server), std::stoi(port),
                                                              NULL, NULL, INTERNET_SERVICE_HTTP, 0, 0));
    if (ic.get() == NULL) {
        return "";
    }

    std::unique_ptr<HINTERNET, InetCloser> hreq(
        HttpOpenRequest(ic.get(), L"POST", StringToWideString(fullPathFile), NULL, NULL, NULL, 0, 0));
    if (hreq.get() == NULL) {
        return "";
    }
    std::string szHeadersS(szHeaders);

    if (!HttpAddRequestHeaders(hreq.get(), StringToWideString(szHeadersS), -1,
                               HTTP_ADDREQ_FLAG_REPLACE | HTTP_ADDREQ_FLAG_ADD)) {
        return "";
    }

    size_t sContentSize = strlen(szContent);
    size_t sEndDataSize = strlen(szEndData);
    size_t dwFileSize = size;

    INTERNET_BUFFERS bufferIn = {};
    bufferIn.dwStructSize = sizeof(INTERNET_BUFFERS);
    bufferIn.dwBufferTotal = sContentSize + dwFileSize + sEndDataSize;

    if (!HttpSendRequestEx(hreq.get(), &bufferIn, NULL, HSR_INITIATE, 0)) {
        return "";
    }

    if (!WriteToInternet(hreq.get(), szContent, sContentSize)) {
        return "";
    }

    if (!WriteToInternet(hreq.get(), data, size)) {
        return "";
    }

    if (!WriteToInternet(hreq.get(), szEndData, sEndDataSize)) {
        return "";
    }

    if (!HttpEndRequest(hreq.get(), NULL, HSR_INITIATE, 0)) {
        logger::info("HttpEndRequest Error");
    } else {
    }

    const int bufferSize = 4096;
    char buffer[bufferSize];
    DWORD bytesRead = 0;
    std::string response;

    while (InternetReadFile(hreq.get(), buffer, bufferSize, &bytesRead) && bytesRead > 0) {
        response.append(buffer, bytesRead);
    }

    if (response.starts_with("<")) {
        response.assign("...");
        logger::info("Error using http://{}", server + ":" + port + "/" + path);
    }
    // Clean up
    InternetCloseHandle(hreq.get());
    InternetCloseHandle(ic.get());
    InternetCloseHandle(io.get());

    return response;
}
*/

std::string HTTPUploader::UploadImage(const char *data, int size, std::string hints) {
    const char *szHeaders = "Content-Type: multipart/form-data; boundary=----974767299852498929531610575";
    const char *szContent =
        "------974767299852498929531610575\r\nContent-Disposition: form-data; name=\"file\"; "
        "filename=\"bmpData.bmp\"\r\nContent-Type: image/bmp\r\n\r\n";
    const char *szEndData = "\r\n------974767299852498929531610575--\r\n";

    std::string server = Conf::getInstance().getServer();
    std::string port = Conf::getInstance().getPort();
    std::string path = Conf::getInstance().getPath();
    auto pos = path.find("comm.php");
    if (pos != std::string::npos) {
        if (MutexGetScreenShotSendMode() == 0)
            path.replace(pos, 8, "itt.php?stuff");
        else if (MutexGetScreenShotSendMode() == 1)
            path.replace(pos, 8, "pic.php?stuff");
        else if (MutexGetScreenShotSendMode() == 2)
            path.replace(pos, 8, "upl.php?stuff");
        else if (MutexGetScreenShotSendMode() == 3)
            path.replace(pos, 8, "item_image.php?stuff");
    }

    path.append("&hints=" + hints);
    logger::info("Using ITT: {}:{}{}", server, port, path);

    // Convert server and path to wide strings
    std::wstring wideServer(server.begin(), server.end());
    std::wstring widePath(path.begin(), path.end());
    std::wstring wideHeaders(szHeaders, szHeaders + strlen(szHeaders));

    // Open WinHTTP session
    HINTERNET hSession = WinHttpOpen(L"HTTPUploader", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, NULL, NULL, 0);
    if (!hSession) {
        logger::debug("WinHttpOpen failed");
        return "";
    }

    // Establish connection
    HINTERNET hConnect = WinHttpConnect(hSession, wideServer.c_str(), std::stoi(port), 0);
    if (!hConnect) {
        logger::debug("WinHttpConnect failed");
        WinHttpCloseHandle(hSession);
        return "";
    }

    // Open request
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", widePath.c_str(), NULL, WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!hRequest) {
        logger::debug("WinHttpOpenRequest failed");
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    // Add headers
    if (!WinHttpAddRequestHeaders(hRequest, wideHeaders.c_str(), -1, WINHTTP_ADDREQ_FLAG_ADD)) {
        logger::debug("WinHttpAddRequestHeaders failed");
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    // Prepare request data
    std::vector<char> requestBody;
    requestBody.insert(requestBody.end(), szContent, szContent + strlen(szContent));
    requestBody.insert(requestBody.end(), data, data + size);
    requestBody.insert(requestBody.end(), szEndData, szEndData + strlen(szEndData));

    // Send request
    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, requestBody.data(), requestBody.size(),
                            requestBody.size(), 0)) {
        logger::debug("WinHttpSendRequest failed");
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    // Wait for response
    if (!WinHttpReceiveResponse(hRequest, NULL)) {
        logger::debug("WinHttpReceiveResponse failed");
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    // Read response
    const int bufferSize = 4096;
    char buffer[bufferSize];
    DWORD bytesRead = 0;
    std::string response;

    do {
        if (!WinHttpReadData(hRequest, buffer, bufferSize, &bytesRead)) {
            logger::debug("WinHttpReadData failed");
            break;
        }
        if (bytesRead > 0) {
            response.append(buffer, bytesRead);
        }
    } while (bytesRead > 0);

    logger::debug("Raw response: {}", response);

    if (response.starts_with("<")) {
        response = "...";
        logger::info("Error using http://{}:{}{}", server, port, path);
    }

    // Clean up
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return response;
}
