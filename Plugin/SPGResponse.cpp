#include "SPGResponse.h"

#include <list>
#include <mutex>
#include <string>
#include <unordered_map>

#include "RE/Skyrim.h"
#include "Misc.h"
namespace logger = SKSE::log;

static const ResponseItem defaultNullResponseItem = {"", 0, "", false};
static const auto qTtl=3000000000000;


std::string trim(const std::string& str) {
    size_t start = str.find_first_not_of(" \t\r\n");
    size_t end = str.find_last_not_of(" \t\r\n");

    if (start == std::string::npos || end == std::string::npos) return "";  // Empty or whitespace-only string

    return str.substr(start, end - start + 1);
}

SPGResponse& SPGResponse::getInstance() {
    static SPGResponse instance;
    return instance;
}

void SPGResponse::decodeAndEnqueue(const std::string& data, bool rechatGenerated) {
    std::stringstream ss(data);
    std::string line;
    while (std::getline(ss, line)) {
        std::stringstream ss(line);
        std::string item;
        
        if (line.find("|") == std::string::npos) continue;

        std::getline(ss, item, '|');
        auto actorname = item;
        std::getline(ss, item, '|');
        auto action = item;
        std::getline(ss, item, '|');
        auto message = item;

        std::string tkey(actorname + action);
        if (tkey.empty()) return;
        auto now = std::chrono::high_resolution_clock::now().time_since_epoch();
        auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();

        ResponseItem fitem = {message, nanos, actorname, rechatGenerated};
        
        this->enqueue(action, fitem);
        
    }
}

void SPGResponse::moveFirstToLast(const std::string& key) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::deque<ResponseItem>& responseQueue = m_responses[key];

    if (!responseQueue.empty()) {
        ResponseItem firstItem = responseQueue.front();
        responseQueue.pop_front();
        responseQueue.push_back(firstItem);
        logger::info("Moved to last {},{}", key, m_responses[key].size());
    }
}

void SPGResponse::eraseOldItems(const std::string& key) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto& queue = m_responses[key];

    auto now = std::chrono::high_resolution_clock::now().time_since_epoch();
    auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();

    auto oldestTimestamp = nanos - qTtl;
    // Remove all items from the queue that are older than oldestTimestamp
    while (!queue.empty() && queue.front().timestamp < oldestTimestamp) {
        queue.pop_front();
        logger::info("Expired {},{}", key, m_responses[key].size());

    }
}

void SPGResponse::enqueue(const std::string& key, const ResponseItem& item) {
    std::lock_guard<std::mutex> lock(m_mutex);

    m_responses[key].push_back(item);
    logger::info("Pushed {},{},{},{}", key, m_responses[key].size(),item.text,item.actor);
}

void SPGResponse::dequeue(const std::string& key) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_responses[key].size() < 1) return;

    auto itemDeleted = m_responses[key].at(0);

    m_responses[key].pop_back();
    logger::info("Popped {},{},{}", key, m_responses[key].size(), itemDeleted.text);

    return;
}

void SPGResponse::dequeueFirst(const std::string& key) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_responses[key].size() < 1) return;
    auto itemDeleted = m_responses[key].at(0);

    m_responses[key].erase(m_responses[key].begin());
    logger::info("Popped {},{},{}", key, m_responses[key].size(), itemDeleted.text);

    return;
}

std::deque<ResponseItem> SPGResponse::dequeue(const std::string& key, const std::string& text) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::deque<ResponseItem>& responseQueue = m_responses[key];
    std::deque<ResponseItem> filteredItems;

    for (auto it = responseQueue.begin(); it != responseQueue.end();) {
        if (it->text == text) {
            filteredItems.push_back(*it);
            it = responseQueue.erase(it);
        } else {
            ++it;
        }
    }

    return filteredItems;
}

ResponseItem SPGResponse::getLastItem(const std::string& key) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::deque<ResponseItem>& responseQueue = m_responses[key];

    if (!responseQueue.empty()) {
        for (auto it = responseQueue.rbegin(); it != responseQueue.rend(); ++it) {
            auto now = std::chrono::high_resolution_clock::now().time_since_epoch();
            auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
            if (key != "HerikaAASPGDialogueHerika3Branch1Topic") { // One item queue, no expiry
                if ((nanos - it->timestamp) > qTtl) {
                    // Too old
                    continue;
                }
            }

            return *it;
        }
    }

    return ResponseItem{"", 0, "", false};
}


bool SPGResponse::findInQueue(const std::string& key,std::string needle) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::deque<ResponseItem>& responseQueue = m_responses[key];

    if (!responseQueue.empty()) {
        for (auto it = responseQueue.rbegin(); it != responseQueue.rend(); ++it) {
            auto entry = *it;
            if (entry.text.contains(needle))
                return true;

          
        }
        return false;
    }

     return false;
}

int SPGResponse::getSize(const std::string& key) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::deque<ResponseItem>& responseQueue = m_responses[key];

    return responseQueue.size();
}

void SPGResponse::markUnFinished(bool t) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    unfinished = t;
    
}

bool SPGResponse::isUnfinished() {
    std::lock_guard<std::mutex> lock(m_mutex);

    return unfinished;
}



ResponseItem SPGResponse::getFirstItem(const std::string& key) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::deque<ResponseItem>& responseQueue = m_responses[key];

    ResponseItem cursor;
    if (!responseQueue.empty()) {
        for (auto it = responseQueue.begin(); it != responseQueue.end(); ++it) {
            auto now = std::chrono::high_resolution_clock::now().time_since_epoch();
            auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
            if (key != "HerikaAASPGDialogueHerika3Branch1Topic") {  // One item queue, no expiry
                if ((nanos - it->timestamp) > qTtl) {
                    // Too old
                    continue;
                }
            }
           // logger::info("{}",it->text);
            
         cursor = *it;
         return cursor;

        }
    }

    return cursor;
}

void SPGResponse::clearQueue(const std::string& key) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_responses.find(key);
    if (it != m_responses.end()) {
        it->second.clear();
    }
}

void SPGResponse::clearAllQueues() {
    std::lock_guard<std::mutex> lock(m_mutex);
    int counter;
    for (auto it = m_responses.begin(); it != m_responses.end(); ++it) {
        it->second.clear();
    }
    m_responses.clear();
    logger::info("All queues cleared");
}

void SPGResponse::clearRechatItems() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& [key, responseQueue] : m_responses) {
        std::erase_if(responseQueue, [](const ResponseItem& item) { return item.rechatGenerated; });
    }
}
