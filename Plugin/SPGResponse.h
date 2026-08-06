#pragma once

#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

struct ResponseItem {
    std::string text;
    long long timestamp;
    std::string actor;
    bool rechatGenerated = false;
};

class SPGResponse {
public:
    static SPGResponse& getInstance();

    void enqueue(const std::string& key, const ResponseItem& item);
    void dequeue(const std::string& key);
    void dequeueFirst(const std::string& key);
    std::deque<ResponseItem> dequeue(const std::string& key, const std::string& text);
    ResponseItem getLastItem(const std::string& key);
    int getSize(const std::string& key);
    ResponseItem getFirstItem(const std::string& key);
    void decodeAndEnqueue(const std::string& data, bool rechatGenerated = false);
    void eraseOldItems(const std::string& key);
    void clearQueue(const std::string& key);
    void moveFirstToLast(const std::string& key);
    void clearAllQueues();
    void clearRechatItems();

    void markUnFinished(bool t);
    bool isUnfinished();


    bool findInQueue(const std::string& key, std::string needle);
    private:
    SPGResponse() {}
    SPGResponse(const SPGResponse&) = delete;
    SPGResponse& operator=(const SPGResponse&) = delete;

    std::mutex m_mutex;
    std::unordered_map<std::string, std::deque<ResponseItem>> m_responses;
    bool unfinished = false;
};

class SPGMessage {
public:
    static SPGMessage& getInstance() {
        static SPGMessage instance;
        return instance;
    }

    void enqueue(const std::string& message) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.push(message);
    }

    void enqueue2(const std::string& message) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue2.push(message);
    }

    void enqueue3(const std::string& message) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue3.push(message);
    }

    std::string dequeue() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_queue.empty()) {
            return "";
        }
        std::string message = m_queue.front();
        m_queue.pop();
        return message;
    }

    std::string dequeue2() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_queue2.empty()) {
            return "";
        }
        std::string message = m_queue2.front();
        m_queue2.pop();
        return message;
    }

    std::string dequeue3() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_queue3.empty()) {
            return "";
        }
        std::string message = m_queue3.front();
        m_queue3.pop();
        return message;
    }

private:
    std::queue<std::string> m_queue;
    std::queue<std::string> m_queue2;
    std::queue<std::string> m_queue3;
    std::mutex m_mutex;

    SPGMessage() = default;
    ~SPGMessage() = default;
    SPGMessage(const SPGMessage&) = delete;
    SPGMessage& operator=(const SPGMessage&) = delete;
};

// Helper trim
std::string trim(const std::string& str);
