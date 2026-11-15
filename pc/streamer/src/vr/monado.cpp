// RinCast's Monado service, found, started and stopped (monado.h).
#include "monado.h"

#include <mutex>

#include "../util.h"
#include "rc/platform.h"

#ifndef _WIN32
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace rcs::monado {

namespace fs = std::filesystem;

namespace {

#ifdef _WIN32
constexpr const char* kServiceName = "rincast-monado.exe";
#else
constexpr const char* kServiceName = "rincast-monado";
#endif

std::mutex g_mutex;
std::string g_display;  // what the running service was started for
#ifdef _WIN32
HANDLE g_process = nullptr;
#else
pid_t g_pid = 0;
#endif

}  // namespace

fs::path Directory() {
    std::error_code ec;
    for (const fs::path& dir : {ExeDir() / "monado", ExeDir() / ".." / "monado" / "monado",
                                ExeDir() / ".." / ".." / "monado" / "monado"}) {
        if (fs::exists(dir / kServiceName, ec)) return fs::weakly_canonical(dir, ec);
    }
    return ExeDir() / "monado";
}

fs::path ServicePath() { return Directory() / kServiceName; }
fs::path Manifest() { return Directory() / "rincast-monado.json"; }

bool IsAvailable() {
    std::error_code ec;
    return fs::exists(ServicePath(), ec) && fs::exists(Manifest(), ec);
}

bool IsRunning() {
    std::lock_guard lock(g_mutex);
#ifdef _WIN32
    if (g_process && WaitForSingleObject(g_process, 0) == WAIT_TIMEOUT) return true;
    // Started by a game through the OpenXR runtime instead of by us.
    return IsProcessRunning(L"rincast-monado.exe");
#else
    if (g_pid && waitpid(g_pid, nullptr, WNOHANG) == 0) return true;
    g_pid = 0;
    return false;
#endif
}

bool Start(const std::string& display) {
    if (IsRunning()) {
        std::string startedFor;
        {
            std::lock_guard lock(g_mutex);
            startedFor = g_display;
        }
        // One a game started (Windows) or ours for this display: keep it.
        if (startedFor.empty() || startedFor == display) return true;
        RC_LOGI("Monado: the headset's display changed; restarting it");
        Stop();
    }
    if (!IsAvailable()) {
        RC_LOGW("Monado: not found in %s", Directory().string().c_str());
        return false;
    }
    std::lock_guard lock(g_mutex);
    g_display = display;
    const fs::path exe = ServicePath();
#ifdef _WIN32
    if (g_process) CloseHandle(g_process);
    g_process = nullptr;
    std::wstring cmd = L"\"" + exe.wstring() + L"\"";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        exe.parent_path().c_str(), &si, &pi)) {
        RC_LOGE("Monado: couldn't start %s (%lu)", exe.string().c_str(), GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    g_process = pi.hProcess;
    RC_LOGI("Monado: started (pid %lu)", pi.dwProcessId);
#else
    // Its log goes to the streamer's data folder; the terminal stays the streamer's. Opened
    // before fork: the child of a threaded process may only make plain system calls.
    std::error_code ec;
    fs::create_directories(DataDir() / "logs", ec);
    const int log = open((DataDir() / "logs" / "monado.log").c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    const pid_t pid = fork();
    if (pid < 0) {
        if (log >= 0) close(log);
        return false;
    }
    if (pid == 0) {
        if (log >= 0) {
            dup2(log, STDOUT_FILENO);
            dup2(log, STDERR_FILENO);
        }
        execl(exe.c_str(), exe.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    if (log >= 0) close(log);
    g_pid = pid;
    RC_LOGI("Monado: started (pid %d)", int(pid));
#endif
    return true;
}

void Stop() {
    std::lock_guard lock(g_mutex);
    g_display.clear();
#ifdef _WIN32
    if (!g_process) return;
    TerminateProcess(g_process, 0);
    WaitForSingleObject(g_process, 2000);
    CloseHandle(g_process);
    g_process = nullptr;
#else
    if (!g_pid) return;
    kill(g_pid, SIGTERM);
    for (int i = 0; i < 20 && waitpid(g_pid, nullptr, WNOHANG) == 0; i++) usleep(100'000);
    if (waitpid(g_pid, nullptr, WNOHANG) == 0) {
        kill(g_pid, SIGKILL);
        waitpid(g_pid, nullptr, 0);
    }
    g_pid = 0;
#endif
    RC_LOGI("Monado: stopped");
}

}  // namespace rcs::monado
