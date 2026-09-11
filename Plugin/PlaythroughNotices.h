#pragma once
#include <algorithm>
#include <deque>
#include <map>
#include <mutex>
#include <string>

// Receives fixed server status codes on HTTP threads; the game thread drains HUD messages.
namespace PlaythroughNotices {
struct Notice { std::string id; std::string text; bool error = false; };
struct State {
    std::mutex mutex;
    std::map<std::string, int> seen;
    std::deque<std::string> order;
    std::deque<Notice> pending;
};
inline State& Get() { static State state; return state; }

inline bool Accept(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return false;
    value = value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
    if (value.size() > 80 || value.compare(0,3,"v1;") != 0 || value.size() < 37 || value[35] != ';') return false;
    const std::string id = value.substr(3,32);
    if (id.find_first_not_of("0123456789abcdef") != std::string::npos) return false;
    const std::string status = value.substr(36);
    Notice notice; notice.id = id;
    int rank = 0;
    if (status == "failed") { rank = 1; notice.error = true; notice.text = "Couldn't create a new Playthrough Save. No data has been rolled back."; }
    else if (status == "created") { rank = 2; notice.text = "New Playthrough Save created."; }
    else if (status == "rollback_failed") { rank = 3; notice.error = true; notice.text = "Playthrough Save created, but rollback couldn't finish. Mod processing continues."; }
    else if (status == "resumed") { rank = 4; notice.text = "New Playthrough Save created."; }
    else return false;
    auto& state = Get();
    std::lock_guard<std::mutex> lock(state.mutex);
    auto found = state.seen.find(id);
    if (found != state.seen.end() && found->second >= rank) return false;
    if (found == state.seen.end()) {
        state.order.push_back(id);
        if (state.order.size() > 32) { state.seen.erase(state.order.front()); state.order.pop_front(); }
    }
    state.seen[id] = rank;
    for (auto& pending : state.pending) if (pending.id == id) { pending = notice; return true; }
    if (state.pending.size() == 8) state.pending.pop_front();
    state.pending.push_back(notice);
    return true;
}

// Call only after the game's loading screen has closed, from its existing update loop.
inline bool Take(Notice& notice) {
    auto& state = Get();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.pending.empty()) return false;
    notice = state.pending.front(); state.pending.pop_front();
    return true;
}

// CHIM's socket transport exposes a raw HTTP header block instead of WinHTTP headers.
inline bool AcceptHeaders(const std::string& headers) {
    const auto end = headers.find("\r\n\r\n");
    const auto limit = end == std::string::npos ? headers.size() : end;
    std::size_t start = 0;
    while (start < limit) {
        auto lineEnd = headers.find("\r\n",start);
        if (lineEnd == std::string::npos) lineEnd = limit;
        const auto colon = headers.find(':',start);
        if (colon < lineEnd) {
            std::string name = headers.substr(start,colon-start);
            std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c) { return c>='A' && c<='Z' ? static_cast<char>(c+32) : static_cast<char>(c); });
            if (name == "x-playthrough-save") { return Accept(headers.substr(colon+1,lineEnd-colon-1)); }
        }
        start = lineEnd + 2;
    }
    return false;
}
}
