#pragma once
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#pragma comment(lib, "ws2_32.lib")

#ifndef CONF_H
    #define CONF_H

#include <string>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>
#include <algorithm>
#include <cctype>

// Function prototypes
bool readIniFile(const std::string& filename, std::string& server, std::string& path, std::string& port, std::string& polint);
bool discoverServerFromProxy(std::string& server, std::string& port);
std::string Sanitize(const std::string& str);
std::vector<std::string> splitString(const std::string& input, char delimiter);

class Conf {
public:
    static Conf& getInstance();

    std::string getServer() const;
    void setServer(const std::string& server);

    std::string getPath() const;
    void setPath(const std::string& path);

    std::string getPort() const;
    void setPort(std::string port);

    std::string getPolint() const;
    void setPolint(std::string port);

    std::string getPathStream();

    bool isOk();
    void setOk(bool ok);

    bool ping();//Check if port is open and reachable

private:
    Conf();
    Conf(const Conf&) = delete;
    Conf& operator=(const Conf&) = delete;

    std::string server_;
    std::string path_;
    std::string port_;
    std::string polint_;
    bool _isOk;
};

#endif /* CONF_H */
