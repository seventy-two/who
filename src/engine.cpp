#include "engine.h"

#include "net.h"

namespace who {

Engine::Engine(std::wstring exe, std::wstring dataDir, std::function<void()> wake)
    : exe_(std::move(exe)), dataDir_(std::move(dataDir)), wake_(std::move(wake)) {
    // The engine dies with Who, however Who ends.
    job_ = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &li, sizeof(li));
}

Engine::~Engine() {
    // Closing its stdin asks it to disconnect cleanly; give it a moment.
    if (in_) {
        CloseHandle(in_);
        in_ = nullptr;
    }
    if (process_) {
        WaitForSingleObject(process_, 2000);
        TerminateProcess(process_, 0);
    }
    if (reader_.joinable()) reader_.join();
    if (process_) CloseHandle(process_);
    if (job_) CloseHandle(job_);
}

std::string Engine::Error() const {
    std::lock_guard<std::mutex> lock(mu_);
    return error_;
}

void Engine::Start() {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE inRead = nullptr, inWrite = nullptr, outRead = nullptr, outWrite = nullptr;
    if (!CreatePipe(&inRead, &inWrite, &sa, 0) || !CreatePipe(&outRead, &outWrite, &sa, 0)) {
        std::lock_guard<std::mutex> lock(mu_);
        error_ = "couldn't make pipes";
        return;
    }
    // Our ends aren't inherited.
    SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = inRead;
    si.hStdOutput = outWrite;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    std::wstring cmd = L"\"" + exe_ + L"\" -data \"" + dataDir_ + L"\"";
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                             nullptr, &si, &pi);
    CloseHandle(inRead);
    CloseHandle(outWrite);
    if (!ok) {
        CloseHandle(inWrite);
        CloseHandle(outRead);
        std::lock_guard<std::mutex> lock(mu_);
        error_ = "couldn't start who-engine.exe (" + std::to_string(GetLastError()) + ")";
        return;
    }
    AssignProcessToJobObject(job_, pi.hProcess);
    SetPriorityClass(pi.hProcess, BELOW_NORMAL_PRIORITY_CLASS);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    process_ = pi.hProcess;
    in_ = inWrite;
    running_ = true;
    reader_ = std::thread(&Engine::readLoop, this, outRead, pi.hProcess);
}

void Engine::readLoop(HANDLE out, HANDLE process) {
    std::string buf;
    char chunk[65536];
    for (;;) {
        DWORD n = 0;
        if (!ReadFile(out, chunk, sizeof(chunk), &n, nullptr) || n == 0) break;
        buf.append(chunk, n);
        size_t start = 0, nl;
        bool any = false;
        while ((nl = buf.find('\n', start)) != std::string::npos) {
            std::string line = buf.substr(start, nl - start);
            start = nl + 1;
            try {
                auto j = nlohmann::json::parse(line);
                std::lock_guard<std::mutex> lock(mu_);
                inbox_.push_back(std::move(j));
                any = true;
            } catch (...) {
            }
        }
        buf.erase(0, start);
        if (any) wake_();
    }
    CloseHandle(out);
    DWORD code = 0;
    WaitForSingleObject(process, 3000);
    GetExitCodeProcess(process, &code);
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (error_.empty()) error_ = "the WhatsApp engine stopped (exit " + std::to_string(code) + ")";
        inbox_.push_back({{"t", "stopped"}, {"code", code}});
    }
    running_ = false;
    wake_();
}

void Engine::Send(const nlohmann::json& cmd) {
    std::lock_guard<std::mutex> lock(sendMu_);
    if (!in_ || !running_) return;
    std::string line = cmd.dump() + "\n";
    DWORD n = 0;
    WriteFile(in_, line.data(), DWORD(line.size()), &n, nullptr);
}

void Engine::Drain(std::vector<nlohmann::json>& out) {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& j : inbox_) out.push_back(std::move(j));
    inbox_.clear();
}

}  // namespace who
