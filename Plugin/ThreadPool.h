#pragma once
#include "PlaythroughSession.h"

#include <SKSE/Events.h>

#include <queue>
#include <mutex>
#include <vector>
#include <thread>
#include <functional>
#include <condition_variable>
#include <chrono>
#include <unordered_map>
#include <sstream>
#include <fmt/format.h>
#include <future>
#include <atomic>

// Windows headers for SEH exception handling
#include <windows.h>
#include <eh.h>
#include <dbghelp.h>
#include <RE/S/SendHUDMessage.h>

template <> 
struct fmt::formatter<std::thread::id> {
    constexpr auto parse(format_parse_context& ctx) -> decltype(ctx.begin()) {
        return ctx.begin();
    }

    template <typename FormatContext>
    auto format(const std::thread::id& id, FormatContext& ctx) const -> decltype(ctx.out()) {
        std::ostringstream oss;
        oss << id;
        return fmt::format_to(ctx.out(), "{}", oss.str());
    }
};

// Helper function to get SEH exception information
inline std::string GetSEHExceptionInfo(unsigned int code, _EXCEPTION_POINTERS* ep) {
    // Common exception codes
    std::string exceptionType;
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:
            exceptionType = "SEH: Access Violation";
            break;
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
            exceptionType = "SEH: Array Bounds Exceeded";
            break;
        case EXCEPTION_BREAKPOINT:
            exceptionType = "SEH: Breakpoint";
            break;
        case EXCEPTION_DATATYPE_MISALIGNMENT:
            exceptionType = "SEH: Datatype Misalignment";
            break;
        case EXCEPTION_FLT_DENORMAL_OPERAND:
            exceptionType = "SEH: Float Denormal Operand";
            break;
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:
            exceptionType = "SEH: Float Divide By Zero";
            break;
        case EXCEPTION_FLT_INEXACT_RESULT:
            exceptionType = "SEH: Float Inexact Result";
            break;
        case EXCEPTION_FLT_INVALID_OPERATION:
            exceptionType = "SEH: Float Invalid Operation";
            break;
        case EXCEPTION_FLT_OVERFLOW:
            exceptionType = "SEH: Float Overflow";
            break;
        case EXCEPTION_FLT_STACK_CHECK:
            exceptionType = "SEH: Float Stack Check";
            break;
        case EXCEPTION_FLT_UNDERFLOW:
            exceptionType = "SEH: Float Underflow";
            break;
        case EXCEPTION_ILLEGAL_INSTRUCTION:
            exceptionType = "SEH: Illegal Instruction";
            break;
        case EXCEPTION_IN_PAGE_ERROR:
            exceptionType = "SEH: In Page Error";
            break;
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
            exceptionType = "SEH: Integer Divide By Zero";
            break;
        case EXCEPTION_INT_OVERFLOW:
            exceptionType = "SEH: Integer Overflow";
            break;
        case EXCEPTION_INVALID_DISPOSITION:
            exceptionType = "SEH: Invalid Disposition";
            break;
        case EXCEPTION_NONCONTINUABLE_EXCEPTION:
            exceptionType = "SEH: Noncontinuable Exception";
            break;
        case EXCEPTION_PRIV_INSTRUCTION:
            exceptionType = "SEH: Privileged Instruction";
            break;
        case EXCEPTION_SINGLE_STEP:
            exceptionType = "SEH: Single Step";
            break;
        case EXCEPTION_STACK_OVERFLOW:
            exceptionType = "SEH: Stack Overflow";
            break;
        default:
            exceptionType = fmt::format("SEH: Unknown Exception (0x{:08X})", code);
            break;
    }

    std::string details;
    if (ep) {
        if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
            // For access violations, we can get the memory address and operation type
            const char* opType = ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read";
            void* address = reinterpret_cast<void*>(ep->ExceptionRecord->ExceptionInformation[1]);
            details = fmt::format("Attempted to {} memory at 0x{:X}", opType, reinterpret_cast<uintptr_t>(address));
        }

        // Add the instruction pointer address
        if (ep->ContextRecord) {
            details += fmt::format(", instruction at 0x{:X}", 
                #ifdef _WIN64
                    ep->ContextRecord->Rip
                #else
                    ep->ContextRecord->Eip
                #endif
            );
        }
    }

    return fmt::format("{}{}", exceptionType, !details.empty() ? (": " + details) : "");
}

// SEH translator function
inline void SEHTranslator(unsigned int code, _EXCEPTION_POINTERS* ep) {
    // Convert SEH exceptions to C++ exceptions with detailed information
    std::string exceptionInfo = GetSEHExceptionInfo(code, ep);  
    throw std::runtime_error(exceptionInfo);
}

class ThreadPool {
private:
    static bool isInitialized;  // Only declaration here

    struct Task {
        std::function<void()> function;
        std::string name;
        std::string key;  // Optional key for task identification (e.g., actor name)
        std::chrono::steady_clock::time_point enqueueTime;
        std::chrono::milliseconds timeout;  // Task timeout duration (0 means no timeout)
        uint64_t taskId;  // Unique identifier for the task
        bool isCancelled;  // Flag to track if task is cancelled

        Task(std::function<void()> f, std::string n, std::string k = "", std::chrono::milliseconds t = std::chrono::milliseconds(-1), uint64_t id = 0) 
            : function(std::move(f)), 
              name(std::move(n)), 
              key(std::move(k)),
              enqueueTime(std::chrono::steady_clock::now()),
              timeout(t),
              taskId(id),
              isCancelled(false) {}
    };

    struct ThreadMetrics {
        std::chrono::steady_clock::time_point startTime;
        size_t tasksCompleted;
        std::chrono::steady_clock::time_point lastTaskTime;
        bool isIdle;
        std::thread::id threadId;
        std::string currentTaskName;
        std::string currentTaskKey;  // Current task's key if any
        size_t timeoutCount;  // Number of tasks that timed out
        size_t errorCount;    // Number of tasks that encountered errors
        std::chrono::milliseconds currentTaskDuration{0};  // Duration of current task
        std::chrono::milliseconds longestTaskDuration{0};  // Duration of longest task
        std::string longestTaskName;  // Name of the longest running task
        std::vector<std::string> recentErrors;  // Store recent error messages (limited size)
        static const size_t MAX_RECENT_ERRORS = 10;  // Maximum number of recent errors to store
        uint64_t currentTaskId = 0;  // Unique identifier for the current task

        ThreadMetrics(
            std::thread::id id = std::thread::id(),
            bool idle = true,
            const std::string& taskName = "none"
        ) : startTime(std::chrono::steady_clock::now()),
            tasksCompleted(0),
            lastTaskTime(std::chrono::steady_clock::now()),
            isIdle(idle),
            threadId(id),
            currentTaskName(taskName),
            currentTaskKey(""),
            timeoutCount(0),
            errorCount(0),
            currentTaskDuration(std::chrono::milliseconds(0)),
            longestTaskDuration(std::chrono::milliseconds(0)),
            longestTaskName("")
        {}

        void addError(const std::string& error, bool isTimeout = false) {
            if (recentErrors.size() >= MAX_RECENT_ERRORS) {
                recentErrors.erase(recentErrors.begin());
            }
            recentErrors.push_back(error);
            errorCount++;
            
            if (isTimeout) {
                timeoutCount++;
            }
        }
    };

    struct TaskTypeMetrics {
        size_t queued{0};
        size_t completed{0};
        size_t timedOut{0};
        size_t errors{0};  // Number of errors for this task type
        size_t cancelled{0};  // Number of cancelled tasks for this type
        std::chrono::milliseconds totalExecutionTime{0};  // Total execution time for this task type
        std::chrono::milliseconds avgExecutionTime{0};    // Average execution time
        std::chrono::milliseconds maxExecutionTime{0};    // Maximum execution time
        std::unordered_map<std::string, size_t> errorsByKey;  // Track errors by task key
        std::unordered_map<std::string, size_t> cancelledByKey;  // Track cancellations by task key
    };

    std::vector<std::thread> workers;
    std::queue<Task> tasks;
    mutable std::mutex queue_mutex;
    mutable std::mutex metrics_mutex;
    std::condition_variable condition;
    bool stop;
    const size_t MAX_THREADS = 48;
    const size_t MAX_QUEUE_SIZE = 1000;  // Maximum number of tasks that can be queued
    const std::chrono::milliseconds DEFAULT_TIMEOUT = std::chrono::seconds(15);  // 15 second default timeout
    std::unordered_map<std::thread::id, ThreadMetrics> threadMetrics;
    std::unordered_map<std::string, TaskTypeMetrics> taskTypeMetrics;  // Track metrics per task type
    size_t totalTasksQueued;
    size_t totalTasksCompleted;
    size_t totalTasksTimedOut;
    size_t totalTasksErrors;
    size_t totalTasksCancelled;  // Track total cancelled tasks
    std::chrono::steady_clock::time_point poolStartTime;
    std::chrono::steady_clock::time_point lastWarningTime;  // Track last warning time
    std::atomic<uint64_t> nextTaskId{1};  // Counter for generating unique task IDs
    std::unordered_map<uint64_t, Task*> activeTasks;  // Track active tasks by ID
    mutable std::mutex activeTasksMutex;  // Mutex for activeTasks access

public:
    ThreadPool() 
        : stop(false), totalTasksQueued(0), totalTasksCompleted(0), totalTasksTimedOut(0), 
          totalTasksErrors(0), totalTasksCancelled(0),
          poolStartTime(std::chrono::steady_clock::now()), lastWarningTime(std::chrono::steady_clock::now()) {
        
        SKSE::log::info("[ThreadPool] Initializing with {} worker threads", MAX_THREADS);
        
        for(size_t i = 0; i < MAX_THREADS; ++i) {
            createWorkerThread();
        }
        isInitialized = true;
    }

    // Static function to check if ThreadPool is initialized
    static bool isPoolInitialized() {
        return isInitialized;
    }

private:
    void createWorkerThread() {
        workers.emplace_back([this] {
            auto threadId = std::this_thread::get_id();
            {
                std::lock_guard<std::mutex> lock(metrics_mutex);
                threadMetrics[threadId] = ThreadMetrics(threadId);
                SKSE::log::info("[ThreadPool] Worker thread {} started", threadId);
            }

            while(true) {
                Task task{nullptr, ""};
                {
                    std::unique_lock<std::mutex> lock(queue_mutex);
                    condition.wait(lock, [this] { return stop || !tasks.empty(); });
                    
                    if(stop && tasks.empty()) {
                        SKSE::log::info("[ThreadPool] Worker thread {} stopping", threadId);
                        return;
                    }

                    task = std::move(tasks.front());
                    tasks.pop();

                    std::lock_guard<std::mutex> metricsLock(metrics_mutex);
                    threadMetrics[threadId].isIdle = false;
                    threadMetrics[threadId].lastTaskTime = std::chrono::steady_clock::now();
                    threadMetrics[threadId].currentTaskName = task.name;
                    threadMetrics[threadId].currentTaskKey = task.key;
                    threadMetrics[threadId].currentTaskId = task.taskId;
                    
                    // Add task to active tasks
                    {
                        std::lock_guard<std::mutex> activeLock(activeTasksMutex);
                        activeTasks[task.taskId] = &task;
                    }
                }

                // Execute the task with timeout if specified
                bool taskTimedOut = false;
                auto taskStartTime = std::chrono::steady_clock::now();
                std::string errorMessage;
                
                if (task.timeout.count() > 0) {  // Only apply timeout if > 0
                    std::future<void> futureTask = std::async(
                        std::launch::async,
                        [&task, &errorMessage]() {
                            // Set up SEH to C++ exception translator for this thread
                            _set_se_translator(SEHTranslator);
                            
                            try {
                                task.function();
                            } catch (const std::exception& e) {
                                errorMessage = fmt::format("Task '{}' execution error: {}", task.name, e.what());
                                SKSE::log::error("[ThreadPool] {}", errorMessage);
                                
                                // Check if this is an SEH exception by looking for the SEH: prefix
                                if (errorMessage.find("SEH: ") != std::string::npos) {
                                    RE::SendHUDMessage::ShowHUDMessage("[CHIM] Caught SEH Exception in task (Prevented CTD). Check logs.");
                                }
                                
                                throw; // Re-throw to mark the future as having an exception
                            } catch (...) {
                                errorMessage = fmt::format("Task '{}' unknown error", task.name);
                                SKSE::log::error("[ThreadPool] {}", errorMessage);
                                throw; // Re-throw to mark the future as having an exception
                            }
                        }
                    );

                    auto status = futureTask.wait_for(task.timeout);
                    if (status == std::future_status::timeout) {
                        taskTimedOut = true;
                        errorMessage = fmt::format("Task '{}' timed out after {} ms", task.name, task.timeout.count());
                        SKSE::log::warn("[ThreadPool] {}", errorMessage);
                        
                        std::lock_guard<std::mutex> lock(metrics_mutex);
                        threadMetrics[threadId].addError(errorMessage, true);  // Explicitly mark as timeout
                        totalTasksTimedOut++;
                        taskTypeMetrics[task.name].timedOut++;
                        taskTypeMetrics[task.name].errors++;  // Count timeout as an error for task type too
                        if (!task.key.empty()) {
                            taskTypeMetrics[task.name].errorsByKey[task.key]++;
                        }
                    } else if (status == std::future_status::ready) {
                        try {
                            // This will re-throw any exception that occurred in the task
                            futureTask.get();
                        } catch (const std::exception& e) {
                            errorMessage = fmt::format("Task '{}' failed in thread {}: {}", task.name, threadId, e.what());
                            SKSE::log::error("[ThreadPool] {}", errorMessage);
                            std::lock_guard<std::mutex> lock(metrics_mutex);
                            threadMetrics[threadId].addError(errorMessage);
                            totalTasksErrors++;
                            taskTypeMetrics[task.name].errors++;
                            if (!task.key.empty()) {
                                taskTypeMetrics[task.name].errorsByKey[task.key]++;
                            }
                        } catch (...) {
                            errorMessage = fmt::format("Task '{}' failed with unknown error in thread {}", task.name, threadId);
                            SKSE::log::error("[ThreadPool] {}", errorMessage);
                            std::lock_guard<std::mutex> lock(metrics_mutex);
                            threadMetrics[threadId].addError(errorMessage);
                            totalTasksErrors++;
                            taskTypeMetrics[task.name].errors++;
                            if (!task.key.empty()) {
                                taskTypeMetrics[task.name].errorsByKey[task.key]++;
                            }
                        }
                    }
                } else {
                    // Execute task with no timeout
                    // Set up SEH to C++ exception translator for this thread
                    _set_se_translator(SEHTranslator);
                    
                    try {
                        task.function();
                    } catch (const std::exception& e) {
                        errorMessage = fmt::format("Task '{}' execution error in thread {}: {}", task.name, threadId, e.what());
                        SKSE::log::error("[ThreadPool] {}", errorMessage);
                        
                        // Check if this is an SEH exception by looking for the SEH: prefix
                        if (errorMessage.find("SEH: ") != std::string::npos) {
                            RE::SendHUDMessage::ShowHUDMessage("[CHIM] Caught SEH Exception in task (Prevented CTD). Check logs.");
                        }
                        
                        std::lock_guard<std::mutex> lock(metrics_mutex);
                        threadMetrics[threadId].addError(errorMessage);
                        totalTasksErrors++;
                        taskTypeMetrics[task.name].errors++;
                        if (!task.key.empty()) {
                            taskTypeMetrics[task.name].errorsByKey[task.key]++;
                        }
                    } catch (...) {
                        errorMessage = fmt::format("Task '{}' unknown error in thread {}", task.name, threadId);
                        SKSE::log::error("[ThreadPool] {}", errorMessage);
                        std::lock_guard<std::mutex> lock(metrics_mutex);
                        threadMetrics[threadId].addError(errorMessage);
                        totalTasksErrors++;
                        taskTypeMetrics[task.name].errors++;
                        if (!task.key.empty()) {
                            taskTypeMetrics[task.name].errorsByKey[task.key]++;
                        }
                    }
                }

                // Remove task from active tasks
                {
                    std::lock_guard<std::mutex> activeLock(activeTasksMutex);
                    activeTasks.erase(task.taskId);
                }

                // Update metrics after task completion
                {
                    std::lock_guard<std::mutex> metricsLock(metrics_mutex);
                    auto taskEndTime = std::chrono::steady_clock::now();
                    auto taskDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
                        taskEndTime - taskStartTime);

                    threadMetrics[threadId].tasksCompleted++;
                    threadMetrics[threadId].isIdle = true;
                    
                    // Update longest task metrics if this task took longer
                    if (taskDuration > threadMetrics[threadId].longestTaskDuration) {
                        threadMetrics[threadId].longestTaskDuration = taskDuration;
                        threadMetrics[threadId].longestTaskName = task.name;
                    }

                    // Update task type metrics
                    if (!taskTimedOut) {
                        totalTasksCompleted++;
                        auto& typeMetrics = taskTypeMetrics[task.name];
                        typeMetrics.completed++;
                        typeMetrics.totalExecutionTime += taskDuration;
                        typeMetrics.avgExecutionTime = typeMetrics.totalExecutionTime / typeMetrics.completed;
                        if (taskDuration > typeMetrics.maxExecutionTime) {
                            typeMetrics.maxExecutionTime = taskDuration;
                        }
                    }
                    
                    threadMetrics[threadId].currentTaskName = "none";
                    threadMetrics[threadId].currentTaskKey = "";  // Clear the key when task is none
                    threadMetrics[threadId].currentTaskDuration = std::chrono::milliseconds(0);
                    threadMetrics[threadId].currentTaskId = 0;  // Clear the task ID
                }
            }
        });
    }

public:
    template<class F>
    void enqueue(const std::string& taskName, F&& f, const std::string& taskKey = "", std::chrono::milliseconds timeout = std::chrono::milliseconds(-1)) {
        if (taskName.empty()) {
            throw std::runtime_error("Task name cannot be empty");
        }

        // Generate unique task ID
        uint64_t taskId = nextTaskId++;

        // If timeout is -1, use the default timeout. If it's 0, it means no timeout.
        auto effectiveTimeout = timeout.count() == -1 ? DEFAULT_TIMEOUT : timeout;

        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            if(stop) throw std::runtime_error("enqueue on stopped ThreadPool");
            
            if (tasks.size() >= MAX_QUEUE_SIZE) {
                auto now = std::chrono::steady_clock::now();
                auto timeSinceLastWarning = std::chrono::duration_cast<std::chrono::seconds>(now - lastWarningTime).count();
                
                if (timeSinceLastWarning >= 30) {  // Only show warning every 30 seconds
                    RE::SendHUDMessage::ShowHUDMessage("[CHIM] Warning: Can't keep up with game load. Check AIAgent.log.");
                    lastWarningTime = now;
                }
                
                SKSE::log::warn("[ThreadPool] Queue is full (size: {}). Dumping thread status before adding new task '{}' (key: {})", 
                    tasks.size(), taskName, taskKey.empty() ? "none" : taskKey);
                lock.unlock();
                logStatus();
                lock.lock();
            }
            
            tasks.emplace([callback = std::forward<F>(f), epoch = PlaythroughSession::Context()]() mutable {
                const PlaythroughSession::Scope scope(epoch);
                callback();
            }, taskName, taskKey, effectiveTimeout, taskId);
            totalTasksQueued++;
            taskTypeMetrics[taskName].queued++;
        }
        condition.notify_one();
    }

    void logStatus() {
        std::lock_guard<std::mutex> lock(metrics_mutex);
        auto now = std::chrono::steady_clock::now();
        auto poolUptime = std::chrono::duration_cast<std::chrono::minutes>(now - poolStartTime).count();

        // Pool summary
        SKSE::log::info(
            "[ThreadPool] Pool Status: Uptime={}m, Queued={}, Completed={}, TimedOut={}, Errors={}, Cancelled={}, QueueSize={}, ActiveThreads={}",
            poolUptime, totalTasksQueued, totalTasksCompleted, totalTasksTimedOut, totalTasksErrors, totalTasksCancelled, tasks.size(), workers.size()
        );

        // Task type statistics
        SKSE::log::info("[ThreadPool] Task Type Statistics:");
        SKSE::log::info("{:<25} | {:>8} | {:>9} | {:>8} | {:>6} | {:>8} | {:>12} | {:>11}",
            "Type", "Queued", "Completed", "TimedOut", "Errors", "Cancelled", "Avg Time(ms)", "Max Time(ms)");
        SKSE::log::info("{:-<25}-+-{:-<8}-+-{:-<9}-+-{:-<8}-+-{:-<6}-+-{:-<8}-+-{:-<12}-+-{:-<11}",
            "", "", "", "", "", "", "", "");
        
        // Create a copy of the metrics for reporting
        auto metricsSnapshot = taskTypeMetrics;
        
        // Report from the snapshot
        for (const auto& [taskType, metrics] : metricsSnapshot) {
            if (metrics.queued > 0) {  // Only show types that have had tasks
                SKSE::log::info("{:<25} | {:>8} | {:>9} | {:>8} | {:>6} | {:>8} | {:>12} | {:>11}", 
                    taskType,
                    metrics.queued,
                    metrics.completed,
                    metrics.timedOut,
                    metrics.errors,
                    metrics.cancelled,
                    metrics.avgExecutionTime.count(),
                    metrics.maxExecutionTime.count()
                );

                // If there are errors by key, report them
                if (!metrics.errorsByKey.empty()) {
                    SKSE::log::info("  Error distribution by key for '{}':", taskType);
                    for (const auto& [key, errorCount] : metrics.errorsByKey) {
                        SKSE::log::info("    {}: {} errors", key, errorCount);
                    }
                }

                // If there are cancellations by key, report them
                if (!metrics.cancelledByKey.empty()) {
                    SKSE::log::info("  Cancellation distribution by key for '{}':", taskType);
                    for (const auto& [key, cancelCount] : metrics.cancelledByKey) {
                        SKSE::log::info("    {}: {} cancellations", key, cancelCount);
                    }
                }
            }
        }

        // Thread details header
        SKSE::log::info("[ThreadPool] Thread Details:");
        SKSE::log::info("{:<10} | {:>9} | {:>10} | {:>9} | {:>6} | {:>11} | {:>7} | {:<25} | {:<50}",
            "ThreadID", "Uptime(m)", "Tasks Done", "TimeOuts", "Errors", "Last Task(s)", "Status", "Longest Task", "Current Task");
        SKSE::log::info("{:-<10}-+-{:-<9}-+-{:-<10}-+-{:-<9}-+-{:-<6}-+-{:-<11}-+-{:-<7}-+-{:-<25}-+-{:-<50}",
            "", "", "", "", "", "", "", "", "");

        // Thread details rows
        for (const auto& [threadId, metrics] : threadMetrics) {
            auto threadUptime = std::chrono::duration_cast<std::chrono::minutes>(now - metrics.startTime).count();
            auto lastTaskAge = std::chrono::duration_cast<std::chrono::seconds>(now - metrics.lastTaskTime).count();
            
            std::ostringstream tidStr;
            tidStr << threadId;
            
            std::string currentTaskInfo = metrics.currentTaskName;
            if (!metrics.currentTaskKey.empty()) {
                currentTaskInfo += fmt::format(" ({})", metrics.currentTaskKey);
            }

            std::string longestTaskInfo = fmt::format("{:>5}ms {}", 
                metrics.longestTaskDuration.count(),
                metrics.longestTaskName);
            
            SKSE::log::info("{:<10} | {:>9} | {:>10} | {:>9} | {:>6} | {:>11} | {:>7} | {:<25} | {:<50}", 
                tidStr.str(),
                threadUptime,
                metrics.tasksCompleted,
                metrics.timeoutCount,
                metrics.errorCount,
                lastTaskAge,
                metrics.isIdle ? "Idle" : "Working",
                longestTaskInfo,
                currentTaskInfo
            );

            // If thread has recent errors, display them
            if (!metrics.recentErrors.empty()) {
                SKSE::log::info("  Recent errors for thread {}:", threadId);
                for (const auto& error : metrics.recentErrors) {
                    SKSE::log::info("    - {}", error);
                }
            }
        }

        // Clear per-task statistics after reporting
        clearTaskStatistics();
    }

    void clearTaskStatistics() {
        // Clear task type metrics
        for (auto& [_, metrics] : taskTypeMetrics) {
            metrics.queued = 0;
            metrics.completed = 0;
            metrics.timedOut = 0;
            metrics.errors = 0;
            metrics.cancelled = 0;
            metrics.totalExecutionTime = std::chrono::milliseconds(0);
            metrics.avgExecutionTime = std::chrono::milliseconds(0);
            metrics.maxExecutionTime = std::chrono::milliseconds(0);
            metrics.errorsByKey.clear();
            metrics.cancelledByKey.clear();
        }

        // Clear thread metrics
        for (auto& [_, metrics] : threadMetrics) {
            metrics.tasksCompleted = 0;
            metrics.timeoutCount = 0;
            metrics.errorCount = 0;
            metrics.currentTaskDuration = std::chrono::milliseconds(0);
            metrics.longestTaskDuration = std::chrono::milliseconds(0);
            metrics.longestTaskName = "";
            metrics.recentErrors.clear();
        }
    }

    ~ThreadPool() {
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            stop = true;
        }
        condition.notify_all();
        for(std::thread &worker: workers) {
            if(worker.joinable()) worker.join();
        }
        SKSE::log::info("[ThreadPool] ThreadPool destroyed. Total tasks processed: {}", totalTasksCompleted);
    }

    static ThreadPool& getInstance() {
        static ThreadPool instance;
        return instance;
    }

    // Get current metrics snapshot
    size_t getQueueSize() const {
        std::lock_guard<std::mutex> lock(queue_mutex);
        return tasks.size();
    }

    size_t getActiveThreadCount() const {
        std::lock_guard<std::mutex> lock(metrics_mutex);
        size_t active = 0;
        for (const auto& [_, metrics] : threadMetrics) {
            if (!metrics.isIdle) active++;
        }
        return active;
    }

    size_t getTotalTasksCompleted() const {
        std::lock_guard<std::mutex> lock(metrics_mutex);
        return totalTasksCompleted;
    }

    size_t getTotalTasksQueued() const {
        std::lock_guard<std::mutex> lock(metrics_mutex);
        return totalTasksQueued;
    }

    size_t getTotalTasksTimedOut() const {
        std::lock_guard<std::mutex> lock(metrics_mutex);
        return totalTasksTimedOut;
    }

    size_t getTotalTasksErrors() const {
        std::lock_guard<std::mutex> lock(metrics_mutex);
        return totalTasksErrors;
    }

    size_t getTotalTasksCancelled() const {
        std::lock_guard<std::mutex> lock(metrics_mutex);
        return totalTasksCancelled;
    }

    // Get current task ID for the calling thread
    uint64_t getCurrentTaskId() const {
        std::lock_guard<std::mutex> lock(metrics_mutex);
        auto threadId = std::this_thread::get_id();
        auto it = threadMetrics.find(threadId);
        if (it != threadMetrics.end()) {
            return it->second.currentTaskId;
        }
        return 0;
    }

    // Check if current task is cancelled
    bool isCurrentTaskCancelled() const {
        uint64_t taskId = getCurrentTaskId();
        if (taskId == 0) return false;
        
        std::lock_guard<std::mutex> lock(activeTasksMutex);
        auto it = activeTasks.find(taskId);
        if (it != activeTasks.end()) {
            return it->second->isCancelled;
        }
        return false;
    }

    // Cancel tasks by type
    void cancelTasksByType(const std::string& taskType) {
        std::lock_guard<std::mutex> lock(activeTasksMutex);
        for (auto& [taskId, task] : activeTasks) {
            if (task->name == taskType) {
                task->isCancelled = true;
                totalTasksCancelled++;
                taskTypeMetrics[taskType].cancelled++;
                if (!task->key.empty()) {
                    taskTypeMetrics[taskType].cancelledByKey[task->key]++;
                }
                SKSE::log::info("[ThreadPool] Cancelled task '{}' (ID: {})", taskType, taskId);
            }
        }
    }

    // Cancel tasks by key
    void cancelTasksByKey(const std::string& key) {
        std::lock_guard<std::mutex> lock(activeTasksMutex);
        for (auto& [taskId, task] : activeTasks) {
            if (task->key == key) {
                task->isCancelled = true;
                totalTasksCancelled++;
                taskTypeMetrics[task->name].cancelled++;
                taskTypeMetrics[task->name].cancelledByKey[key]++;
                SKSE::log::info("[ThreadPool] Cancelled task '{}' with key '{}' (ID: {})", task->name, key, taskId);
            }
        }
    }

    // Cancel specific task by ID
    void cancelTaskById(uint64_t taskId) {
        std::lock_guard<std::mutex> lock(activeTasksMutex);
        auto it = activeTasks.find(taskId);
        if (it != activeTasks.end()) {
            it->second->isCancelled = true;
            totalTasksCancelled++;
            taskTypeMetrics[it->second->name].cancelled++;
            if (!it->second->key.empty()) {
                taskTypeMetrics[it->second->name].cancelledByKey[it->second->key]++;
            }
            SKSE::log::info("[ThreadPool] Cancelled task '{}' (ID: {})", it->second->name, taskId);
        }
    }

    // Running tasks by type
    int runningTasksByType(const std::string& taskType) {
        std::lock_guard<std::mutex> lock(activeTasksMutex);
        int i = 0;
        for (auto& [taskId, task] : activeTasks) {
            if (task->name == taskType) {
                i++;
            }
        }

        return i;
    }

}; 
