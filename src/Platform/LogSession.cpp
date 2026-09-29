#include "LogSession.h"
#include <atomic>
#include <algorithm>
#include <stdexcept>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <memory>
#include <sstream>
#include <string>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

namespace dyf::Platform::LogInternal
{
    namespace
    {
        namespace fs = std::filesystem;
        constexpr size_t MaxBytes = 5 * 1024 * 1024;
        constexpr unsigned MaxFiles = 5;
        struct Session
        {
            std::once_flag once;
            std::mutex mutex;
            fs::path directory;
            std::ofstream output;
            std::atomic<bool> enabled{false};
            unsigned index = 0;
            size_t bytes = 0;
            unsigned long pid = 0;
#ifdef _WIN32
            HANDLE monitor = nullptr;
#endif
            const char* crash = "disabled";
        };
        Session& State() { static auto* state = new Session; return *state; }

        fs::path EnvironmentPath(const char* name)
        {
#ifdef _WIN32
            const std::string narrow(name);
            const std::wstring wide(narrow.begin(), narrow.end());
            const DWORD size = GetEnvironmentVariableW(wide.c_str(), nullptr, 0);
            if (!size) return {};
            std::wstring value(size, L'\0');
            const DWORD copied = GetEnvironmentVariableW(wide.c_str(), value.data(), size);
            if (!copied || copied >= size) return {};
            value.resize(copied);
            return value;
#else
            const auto* value = std::getenv(name);
            return value && *value ? fs::path(value) : fs::path{};
#endif
        }
        bool IsOff(const char* name) { return EnvironmentPath(name) == fs::path("0"); }
#ifdef _WIN32
        bool RefreshMonitor(Session& state)
        {
            if (state.monitor && WaitForSingleObject(state.monitor, 0) == WAIT_OBJECT_0) {
                CloseHandle(state.monitor);
                state.monitor = nullptr;
                state.crash = "monitor_stopped";
                return true;
            }
            return false;
        }
#endif
        fs::path ExecutablePath()
        {
#ifdef _WIN32
            std::wstring value(32768, L'\0');
            const auto length = GetModuleFileNameW(nullptr, value.data(), static_cast<DWORD>(value.size()));
            if (length == 0 || length == value.size()) return {};
            value.resize(length);
            return value;
#elif defined(__APPLE__)
            uint32_t size = 0;
            _NSGetExecutablePath(nullptr, &size);
            std::string value(size, '\0');
            if (_NSGetExecutablePath(value.data(), &size) != 0) return {};
            return value.c_str();
#else
            char value[4096];
            const auto length = readlink("/proc/self/exe", value, sizeof(value));
            return length > 0 && static_cast<size_t>(length) < sizeof(value)
                ? fs::path(std::string(value, static_cast<size_t>(length))) : fs::path{};
#endif
        }
        void SaveMetadata(bool shutdown)
        {
            auto& state = State();
            const auto temporary = state.directory / "session.tmp";
            std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
            out << "{\"version\":1,\"source\":\"engine-auto\",\"target_pid\":" << state.pid
                << ",\"crash_capture\":\"" << state.crash << "\",\"shutdown_recorded\":"
                << (shutdown ? "true" : "false")
                << ",\"log_pattern\":\"engine-*.log\",\"max_file_bytes\":" << MaxBytes
                << ",\"max_files\":" << MaxFiles << "}\n";
            out.flush();
            if (!out) throw std::runtime_error("session metadata write failed");
            out.close();
#ifdef _WIN32
            if (!MoveFileExW(temporary.c_str(), (state.directory / "session.json").c_str(), MOVEFILE_REPLACE_EXISTING))
                throw std::runtime_error("session metadata replacement failed");
#else
            fs::rename(temporary, state.directory / "session.json");
#endif
        }
        void Finish()
        {
            try {
                auto& state = State();
                std::lock_guard<std::mutex> lock(state.mutex);
#ifdef _WIN32
                RefreshMonitor(state);
                if (state.monitor) { CloseHandle(state.monitor); state.monitor = nullptr; }
#endif
                state.output.flush();
                SaveMetadata(true); // This is orderly cleanup, not proof of exit code 0.
            } catch (...) { std::fputs("dyf log: shutdown metadata could not be saved\n", stderr); }
        }
#ifdef _WIN32
        std::wstring Quote(std::wstring_view value)
        {
            std::wstring out = L"\"";
            size_t slashes = 0;
            for (wchar_t c : value) {
                if (c == L'\\') { ++slashes; continue; }
                out.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
                out += c; slashes = 0;
            }
            out.append(slashes * 2, L'\\');
            out += L'"';
            return out;
        }
        const char* StartMonitor(const fs::path& executable)
        {
            auto& state = State();
            if (IsDebuggerPresent()) return "debugger_present";
            auto monitor = EnvironmentPath("DY_LOG_MONITOR");
            if (monitor.empty()) monitor = executable.parent_path() / "windows_monitor.exe";
            if (!fs::is_regular_file(monitor)) return "unavailable";
            fs::create_directory(state.directory / "crash");
            const auto eventName = L"Local\\dyf-log-" + state.directory.filename().wstring();
            using Handle = std::unique_ptr<void, decltype(&CloseHandle)>;
            Handle ready(CreateEventW(nullptr, TRUE, FALSE, eventName.c_str()), &CloseHandle);
            if (!ready) return "unavailable";
            auto command = Quote(monitor.wstring()) + L" --attach " + Quote((state.directory / "crash").wstring())
                + L" " + std::to_wstring(state.pid) + L" " + Quote(eventName);
            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            PROCESS_INFORMATION process{};
            const BOOL created = CreateProcessW(monitor.c_str(), command.data(), nullptr, nullptr, FALSE,
                CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
            if (!created) return "unavailable";
            Handle processHandle(process.hProcess, &CloseHandle), thread(process.hThread, &CloseHandle);
            HANDLE handles[] = {ready.get(), processHandle.get()};
            const DWORD wait = WaitForMultipleObjects(2, handles, FALSE, 5000);
            if (wait == WAIT_OBJECT_0 || wait == WAIT_TIMEOUT) state.monitor = processHandle.release();
            // Never kill a monitor on timeout: it could be completing the attach handshake.
            return wait == WAIT_OBJECT_0 ? "attached" : wait == WAIT_TIMEOUT ? "starting_timeout" : "unavailable";
        }
#endif
        void Start()
        {
            auto& state = State();
            if (IsOff("DY_LOG_AUTO")) return;
            if (!EnvironmentPath("DY_LOG_SESSION_ID").empty()) {
                state.directory = EnvironmentPath("DY_LOG_SESSION_DIR");
                return; // The external runner already owns storage and crash monitoring.
            }
#ifdef _WIN32
            state.pid = GetCurrentProcessId();
#else
            state.pid = static_cast<unsigned long>(getpid());
#endif
            const auto executable = ExecutablePath();
            auto root = EnvironmentPath("DY_LOG_DIR");
            if (root.empty()) {
#ifdef _WIN32
                root = EnvironmentPath("LOCALAPPDATA");
#elif defined(__APPLE__)
                root = EnvironmentPath("HOME") / "Library" / "Logs";
#else
                root = EnvironmentPath("XDG_STATE_HOME");
                if (root.empty()) root = EnvironmentPath("HOME") / ".local" / "state";
#endif
                if (root.empty()) root = fs::temp_directory_path();
                root /= fs::path("dy_engine") / "logs" / (executable.empty() ? fs::path("application") : executable.stem());
            }
            root = fs::absolute(root);
            fs::create_directories(root);
            const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            const auto base = "session-" + std::to_string(stamp) + "-" + std::to_string(state.pid);
            bool created = false;
            for (unsigned n = 0; n < 100; ++n) {
                state.directory = root / (base + "-" + std::to_string(n));
                if (fs::create_directory(state.directory)) { created = true; break; }
            }
            if (!created) throw std::runtime_error("could not create a unique log session");
            state.enabled.store(true);
            std::atexit(Finish);
            if (!IsOff("DY_LOG_CRASH")) {
#ifdef _WIN32
                try { state.crash = StartMonitor(executable); }
                catch (...) { state.crash = "unavailable"; }
#else
                state.crash = "unavailable";
#endif
                if (std::string_view(state.crash) == "unavailable")
                    std::fputs("dyf log: automatic crash capture unavailable; check windows_monitor.exe or DY_LOG_MONITOR\n", stderr);
            }
            try { SaveMetadata(false); }
            catch (...) { std::fputs("dyf log: initial metadata could not be saved; file logging remains enabled\n", stderr); }
        }
        void NextFile()
        {
            auto& state = State();
            if (state.output.is_open()) state.output.close();
            state.output.clear();
            char name[48];
            std::snprintf(name, sizeof(name), "engine-%08u.log", ++state.index);
            state.output.open(state.directory / name, std::ios::binary | std::ios::trunc);
            if (!state.output) throw std::runtime_error("log file open failed");
            state.bytes = 0;
            if (state.index > MaxFiles) {
                std::snprintf(name, sizeof(name), "engine-%08u.log", state.index - MaxFiles);
                fs::remove(state.directory / name);
            }
        }
    }

    void InitializeSession() noexcept
    {
        try {
            std::call_once(State().once, [] {
                try { Start(); }
                catch (const std::exception& error) {
                    State().enabled.store(false);
                    std::fprintf(stderr, "dyf log: automatic capture setup failed: %s\n", error.what());
                }
            });
        } catch (...) {}
    }
    bool HasSessionOutput() noexcept
    {
        try { return State().enabled.load(); } catch (...) { return false; }
    }
    std::string SessionDirectory()
    {
        InitializeSession();
        return State().directory.u8string();
    }
    void WriteSession(std::string_view text) noexcept
    {
        Session* existing = nullptr;
        try {
            auto& state = State();
            existing = &state;
            if (!state.enabled.load()) return;
            std::lock_guard<std::mutex> lock(state.mutex);
#ifdef _WIN32
            if (RefreshMonitor(state)) {
                try { SaveMetadata(false); } catch (...) {}
            }
#endif
            while (!text.empty()) {
                if (!state.output.is_open() || state.bytes == MaxBytes) NextFile();
                const size_t size = (std::min)(text.size(), MaxBytes - state.bytes);
                state.output.write(text.data(), static_cast<std::streamsize>(size));
                state.output.flush();
                if (!state.output) throw std::runtime_error("log file write failed");
                state.bytes += size;
                text.remove_prefix(size);
            }
        } catch (...) {
            if (existing) existing->enabled.store(false);
            std::fputs("dyf log: file storage failed; console and callbacks remain available\n", stderr);
        }
    }
}
