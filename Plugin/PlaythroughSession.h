#pragma once
#include <cstdint>
#include <functional>
#include <string>

namespace PlaythroughSession {
    std::uint64_t Generation();
    std::uint64_t Context();
    bool Allowed(std::uint64_t generation);
    std::string Header(std::uint64_t generation);
    void BeginLoad();
    void ResetCharacter();
    std::string Character();
    bool NewCharacter();
    void RestoreCharacter(const std::string& value, bool newCharacter = false);
    void Connect(std::function<void()> resume, bool newGame = false);

    // Queued callbacks retain their originating load, including nested requests.
    class Scope {
        std::uint64_t previous;
    public:
        explicit Scope(std::uint64_t generation);
        ~Scope();
    };
}
