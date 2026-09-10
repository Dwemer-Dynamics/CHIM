#pragma once
#include <cstdint>
#include <string>
struct ScriptLine;
namespace DirectorScene {
    std::uint64_t Generation();
    bool Active();
    std::uint64_t BeginRequest();
    void RequestFailed(std::uint64_t token);
    bool ApproveAction(const std::string& command);
    bool ReadyToSpeak();
    bool IsDispatchingAction();
    void ProcessActions();
    void Cancel();
    void Queue(const std::string& encoded);
    void CompleteLine(const ScriptLine& line, bool spoken);
}
