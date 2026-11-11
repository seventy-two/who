// who-engine, the Go program that talks to WhatsApp, run hidden as a child
// process: commands go to its stdin and events come back on its stdout,
// one JSON object a line.
#pragma once

#include <windows.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "nlohmann/json.hpp"

namespace who {

class Engine {
public:
    // wake is called from the reader thread when there are events.
    Engine(std::wstring exe, std::wstring dataDir, std::function<void()> wake);
    ~Engine();

    void Start();
    bool Running() const { return running_; }
    std::string Error() const;

    void Send(const nlohmann::json& cmd);
    // Drain moves the events received so far into out.
    void Drain(std::vector<nlohmann::json>& out);

private:
    void readLoop(HANDLE out, HANDLE process);

    std::wstring exe_, dataDir_;
    std::function<void()> wake_;
    HANDLE job_ = nullptr, process_ = nullptr, in_ = nullptr;
    std::thread reader_;
    std::atomic<bool> running_{false};
    mutable std::mutex mu_;
    std::vector<nlohmann::json> inbox_;
    std::string error_;
    std::mutex sendMu_;
};

}  // namespace who
