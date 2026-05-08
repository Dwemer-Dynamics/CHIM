#include "Conf.h"
#include <winsock2.h>
#include <ws2tcpip.h>

#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

namespace logger = SKSE::log;

std::string Sanitize(const std::string& str) {
    
    auto start = std::find_if_not(str.begin(), str.end(), [](int c) { return std::isspace(c); });
    auto end = std::find_if_not(str.rbegin(), str.rend(), [](int c) { return std::isspace(c); }).base();
    return (start < end) ? std::string(start, end) : std::string();
}


std::vector<std::string> splitString(const std::string& input, char delimiter) {
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream tokenStream(input);

    while (std::getline(tokenStream, token, delimiter)) {
        tokens.push_back(token);
    }

    return tokens;
}


bool readIniFile(const std::string& filename, std::string& server, std::string& path, std::string& port,
                 std::string& polint)  {
    const std::string directoryName = "Data/SKSE/Plugins";

    std::ifstream file(directoryName + "/" + filename);

    if (!file.is_open()) {
        logger::info("Error: failed to open file {} ", directoryName + "/" + filename);
        return false;
    }

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') {
            continue;  // skip empty and comment lines
        }

        size_t delimiter = line.find('=');
        if (delimiter == std::string::npos) {
            logger::info("Error: invalid line format in file {}", filename);
            return false;
        }

        std::string key = line.substr(0, delimiter);
        std::string value = line.substr(delimiter + 1);

        if (key == "SERVER") {
            server = Sanitize(value);
        } else if (key == "PATH") {
            path = Sanitize(value);
        } else if (key == "PORT") {
            port = Sanitize(value);
        } else if (key == "POLINT") {
            polint = Sanitize(value);
        } else {
            logger::info("Warning: unknown key {} in file {} ", key, filename);
        }
    }

    file.close();

    return true;
}

bool discoverServerFromProxy(std::string& server, std::string& port) {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        logger::error("WSAStartup failed for auto-discovery");
        return false;
    }

    SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) {
        logger::error("Failed to create socket for auto-discovery");
        WSACleanup();
        return false;
    }

    // Set 3-second timeout
    DWORD timeout = 3000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (char*)&timeout, sizeof(timeout));

    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(7135);  // CHIM.exe discovery port
    inet_pton(AF_INET, "127.0.0.1", &serverAddr.sin_addr);

    if (connect(sock, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        logger::info("Auto-discovery failed: Cannot connect to CHIM.exe proxy on localhost:7135");
        closesocket(sock);
        WSACleanup();
        return false;
    }

    // Send HTTP GET request
    const char* request = "GET /discover HTTP/1.1\r\nHost: localhost:7135\r\nConnection: close\r\n\r\n";
    if (send(sock, request, strlen(request), 0) == SOCKET_ERROR) {
        logger::error("Failed to send discovery request to CHIM.exe proxy");
        closesocket(sock);
        WSACleanup();
        return false;
    }

    // Read response
    char buffer[1024] = {0};
    int bytesReceived = recv(sock, buffer, sizeof(buffer) - 1, 0);
    closesocket(sock);
    WSACleanup();

    if (bytesReceived <= 0) {
        logger::error("Failed to receive discovery response from CHIM.exe proxy");
        return false;
    }

    std::string response(buffer, bytesReceived);
    
    // Find the HTTP body (after double CRLF)
    size_t bodyStart = response.find("\r\n\r\n");
    if (bodyStart == std::string::npos) {
        logger::error("Invalid HTTP response format from CHIM.exe proxy");
        return false;
    }
    
    std::string body = response.substr(bodyStart + 4);
    body = Sanitize(body);
    
    // Parse IP:PORT format
    size_t colonPos = body.find(':');
    if (colonPos == std::string::npos) {
        logger::error("Invalid discovery response format from CHIM.exe proxy: {}", body);
        return false;
    }
    
    server = Sanitize(body.substr(0, colonPos));
    port = Sanitize(body.substr(colonPos + 1));
    
    // Validate IP format (basic check)
    if (server.empty() || port.empty()) {
        logger::error("Empty server or port from CHIM.exe proxy discovery");
        return false;
    }
    
    logger::info("Auto-discovery successful: {}:{}", server, port);
    return true;
}

/***********************************/
Conf::Conf() : server_(""), path_(""), port_(""), polint_("") {}

Conf& Conf::getInstance() {
    static Conf instance;
    if (instance.getServer().empty()) {
        std::string server;
        std::string path = "/HerikaServer/comm.php";  // Default path
        std::string port;
        std::string polint = "10";

        bool localok = false;

        // Priority 1: Try AIAgent.ini first (backward compatibility)
        if (readIniFile("AIAgent.ini", server, path, port, polint)) {
            localok = true;
            logger::info("Using AIAgent.ini configuration: http://{}:{}{}", server, port, path);
        }
        // Priority 2: Try auto-discovery via CHIM.exe proxy
        else if (discoverServerFromProxy(server, port)) {
            localok = true;
            // Keep default path for discovered servers
            path = "/HerikaServer/comm.php";
            polint = "1";  // Default polling interval for discovered servers
            logger::info("Using auto-discovered configuration: http://{}:{}{}", server, port, path);
        }
        // Priority 3: Fallback to defaults (this is still a valid configuration)
        else {
            server = "127.0.0.1";
            port = "8081";
            path = "/HerikaServer/comm.php";
            polint = "1";
            logger::info("Using default fallback configuration: http://{}:{}{}", server, port, path);
            localok = true;  // Fallback is still a valid configuration
        }

        instance.setServer(server);
        instance.getInstance().setPath(path);
        instance.getInstance().setPort(port);
        instance.getInstance().setPolint(polint);
        instance.getInstance().setOk(localok);
    }
    return instance;
}

std::string Conf::getServer() const { return server_; }
void Conf::setServer(const std::string& server) { server_ = server; }
std::string Conf::getPath() const { return path_; }
void Conf::setPath(const std::string& path) { path_ = path; }
std::string Conf::getPort() const { return port_; }
void Conf::setPort(std::string port) { port_ = port; }

std::string Conf::getPolint() const { return polint_; }
void Conf::setPolint(std::string polint) { polint_ = polint; }

void Conf::setOk(bool ok){ _isOk = ok; }

bool Conf::isOk() { 
    return _isOk;
}


std::string Conf::getPathStream() {
    std::string _pathStream;
    _pathStream.append(path_);

    std::string searchString = "comm.php";
    std::string replaceString = "stream.php";

    size_t pos = _pathStream.find(searchString);
    while (pos != std::string::npos) {
        _pathStream.replace(pos, searchString.length(), replaceString);
        pos = _pathStream.find(searchString, pos + replaceString.length());
    }

    return _pathStream;
}

bool Conf::ping() {
    std::string hostnameStr = getServer();
    std::string portStr = getPort();
    const char* hostname = hostnameStr.c_str();
    const char* port = portStr.c_str();

#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        logger::error("WSAStartup failed");
        return false;
    }
#endif

    struct addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* result = nullptr;

    int res = getaddrinfo(hostname, port, &hints, &result);
    if (res != 0) {
#ifdef _WIN32
        WSACleanup();
#endif
        logger::error("getaddrinfo failed: {}", res);
        return false;
    }

    bool can_connect = false;
    for (struct addrinfo* ptr = result; ptr != nullptr; ptr = ptr->ai_next) {
#ifdef _WIN32
        SOCKET sock = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
        if (sock == INVALID_SOCKET) continue;
        if (connect(sock, ptr->ai_addr, (int)ptr->ai_addrlen) == 0) {
            can_connect = true;
            closesocket(sock);
            break;
        }
        closesocket(sock);
#else
        int sock = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
        if (sock < 0) continue;
        if (connect(sock, ptr->ai_addr, ptr->ai_addrlen) == 0) {
            can_connect = true;
            ::close(sock);
            break;
        }
        ::close(sock);
#endif
    }

    freeaddrinfo(result);

#ifdef _WIN32
    WSACleanup();
#endif

    if (!can_connect) {
        logger::info("Ping: cannot connect to {}:{}", hostname, port);
    } else {
        logger::info("Ping: SUCCESS for {}:{}", hostname, port);
    }

    return can_connect;
}


