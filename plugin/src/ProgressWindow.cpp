#include "ProgressWindow.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#include "Platform.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

// .../Insta360Stitch.ofx.bundle/Contents/Win64/Insta360Stitch.ofx -> .../Contents/Resources/<name>
std::string BundleTool(const std::string& p_Name)
{
    std::string dir = ModuleDir();
    const size_t slash = dir.find_last_of("/\\");
    if (dir.empty() || slash == std::string::npos)
    {
        return "";
    }
    dir.resize(slash);
#ifdef _WIN32
    return JoinPath(JoinPath(dir, "Resources"), p_Name + ".exe");
#else
    return JoinPath(JoinPath(dir, "Resources"), p_Name);
#endif
}

static double Now()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

ProgressWindow::~ProgressWindow()
{
    close();
}

#ifdef _WIN32

void ProgressWindow::open(const std::string& p_Title)
{
    const std::string path = BundleTool("TrackProgress");
    if (path.empty() || !FileExists(path))
    {
        return;
    }
    SECURITY_ATTRIBUTES inherit = {sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE childIn = nullptr, toHelper = nullptr, fromHelper = nullptr, childOut = nullptr;
    if (!CreatePipe(&childIn, &toHelper, &inherit, 0))
    {
        return;
    }
    if (!CreatePipe(&fromHelper, &childOut, &inherit, 0))
    {
        CloseHandle(childIn);
        CloseHandle(toHelper);
        return;
    }
    // Only the helper's ends are inherited.
    SetHandleInformation(toHelper, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(fromHelper, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = childIn;
    startup.hStdOutput = childOut;
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION process = {};
    std::wstring title = Widen(p_Title);
    for (wchar_t& c : title)
    {
        c = c == L'"' ? L'\'' : c;
    }
    std::wstring command = L"\"" + Widen(path) + L"\" \"" + title + L"\"";
    const BOOL started = CreateProcessW(Widen(path).c_str(), &command[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                                        &startup, &process);
    CloseHandle(childIn);
    CloseHandle(childOut);
    if (!started)
    {
        CloseHandle(toHelper);
        CloseHandle(fromHelper);
        return;
    }
    CloseHandle(process.hThread);
    m_Process = process.hProcess;
    m_ToHelper = toHelper;
    m_FromHelper = fromHelper;
}

bool ProgressWindow::update(double p_Fraction, const std::string& p_Text)
{
    if (!m_ToHelper)
    {
        return true;
    }

    DWORD available = 0;
    if (PeekNamedPipe(m_FromHelper, nullptr, 0, nullptr, &available, nullptr) && available > 0)
    {
        char buffer[256];
        DWORD got = 0;
        if (ReadFile(m_FromHelper, buffer, std::min<DWORD>(available, sizeof(buffer) - 1), &got, nullptr) && got > 0)
        {
            buffer[got] = '\0';
            m_Cancelled = m_Cancelled || strstr(buffer, "cancel") != nullptr;
        }
    }

    // About ten updates a second is plenty for a progress bar.
    const double now = Now();
    if (now - m_LastSent >= 0.1)
    {
        m_LastSent = now;
        const std::string line = "p " + std::to_string(p_Fraction) + " " + p_Text + "\n";
        DWORD written = 0;
        if (!WriteFile(m_ToHelper, line.data(), DWORD(line.size()), &written, nullptr))
        {
            close(); // the window was closed some other way; carry on without it
        }
    }
    return !m_Cancelled;
}

void ProgressWindow::close()
{
    if (m_ToHelper)
    {
        const char done[] = "done\n";
        DWORD written = 0;
        WriteFile(m_ToHelper, done, sizeof(done) - 1, &written, nullptr);
        CloseHandle(m_ToHelper);
        m_ToHelper = nullptr;
    }
    if (m_FromHelper)
    {
        CloseHandle(m_FromHelper);
        m_FromHelper = nullptr;
    }
    if (m_Process)
    {
        // Give the window a moment to close by itself, then make sure it is gone.
        if (WaitForSingleObject(m_Process, 200) != WAIT_OBJECT_0)
        {
            TerminateProcess(m_Process, 0);
        }
        CloseHandle(m_Process);
        m_Process = nullptr;
    }
}

#else

void ProgressWindow::open(const std::string& p_Title)
{
    const std::string path = BundleTool("TrackProgress");
    int toHelper[2], fromHelper[2];
    if (path.empty() || access(path.c_str(), X_OK) != 0 || pipe(toHelper) != 0)
    {
        return;
    }
    if (pipe(fromHelper) != 0)
    {
        ::close(toHelper[0]);
        ::close(toHelper[1]);
        return;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, toHelper[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, fromHelper[1], STDOUT_FILENO);
    for (int fd : {toHelper[0], toHelper[1], fromHelper[0], fromHelper[1]})
    {
        posix_spawn_file_actions_addclose(&actions, fd);
    }
    char* argv[] = {const_cast<char*>(path.c_str()), const_cast<char*>(p_Title.c_str()), nullptr};
    pid_t pid = -1;
    const int status = posix_spawn(&pid, path.c_str(), &actions, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&actions);

    ::close(toHelper[0]);
    ::close(fromHelper[1]);
    if (status != 0)
    {
        ::close(toHelper[1]);
        ::close(fromHelper[0]);
        return;
    }
    m_Pid = pid;
    m_ToHelper = toHelper[1];
    m_FromHelper = fromHelper[0];
    signal(SIGPIPE, SIG_IGN); // a closed window must not take the host down
    fcntl(m_FromHelper, F_SETFL, fcntl(m_FromHelper, F_GETFL) | O_NONBLOCK);
}

bool ProgressWindow::update(double p_Fraction, const std::string& p_Text)
{
    if (m_ToHelper < 0)
    {
        return true;
    }

    char buffer[256];
    const ssize_t got = read(m_FromHelper, buffer, sizeof(buffer) - 1);
    if (got > 0)
    {
        buffer[got] = '\0';
        m_Cancelled = m_Cancelled || strstr(buffer, "cancel") != nullptr;
    }

    const double now = Now();
    if (now - m_LastSent >= 0.1)
    {
        m_LastSent = now;
        const std::string line = "p " + std::to_string(p_Fraction) + " " + p_Text + "\n";
        if (write(m_ToHelper, line.data(), line.size()) < 0)
        {
            close();
        }
    }
    return !m_Cancelled;
}

void ProgressWindow::close()
{
    if (m_ToHelper >= 0)
    {
        const char done[] = "done\n";
        (void)!write(m_ToHelper, done, sizeof(done) - 1);
        ::close(m_ToHelper);
        m_ToHelper = -1;
    }
    if (m_FromHelper >= 0)
    {
        ::close(m_FromHelper);
        m_FromHelper = -1;
    }
    if (m_Pid > 0)
    {
        for (int i = 0; i < 20 && waitpid(m_Pid, nullptr, WNOHANG) == 0; ++i)
        {
            usleep(10000);
        }
        if (waitpid(m_Pid, nullptr, WNOHANG) == 0)
        {
            kill(m_Pid, SIGTERM);
            waitpid(m_Pid, nullptr, 0);
        }
        m_Pid = -1;
    }
}

#endif
