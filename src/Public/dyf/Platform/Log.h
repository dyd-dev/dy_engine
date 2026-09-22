#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace dyf::Platform
{
    enum class LogLevel { Debug, Info, Warning, Error, Fatal, Off };

    struct LogRecord
    {
        LogLevel level = LogLevel::Info;
        std::string category;
        std::string message;
        std::string file;
        int line = 0;
        uint64_t sequence = 0;
        std::chrono::system_clock::time_point timestamp;
        std::thread::id thread;
    };

    namespace Log
    {
        // Automatic on first log, Window creation or Device creation. Settings are read once:
        // DY_LOG_AUTO=0 disables automatic files/monitor; DY_LOG_DIR overrides the log root;
        // DY_LOG_CRASH=0 disables crash monitoring; DY_LOG_MONITOR overrides the helper path.
        // Native crash monitoring is Windows x64, from initialization onward. Bundle
        // windows_monitor.exe beside the application. Existing debuggers take precedence.
        // Only common Log messages go to automatic files; use log_runner for all stdout/stderr.
        void Initialize() noexcept;
        std::string GetSessionDirectory();

        // Invoked on the logging thread, outside the output lock. Copy to retain a record.
        // Stop logging workers before replacing/removing a callback or destroying its data.
        // Exceptions are contained. Logging inside a callback does not invoke it recursively.
        using Callback = void (*)(const LogRecord&, void*);
        void SetLevel(LogLevel level) noexcept;
        void SetCallback(Callback callback, void* userData = nullptr) noexcept;
        // Callback registration does not turn stderr off.
        void SetDefaultOutputEnabled(bool enabled) noexcept;
        void Write(LogLevel level, std::string_view category, std::string_view message,
                   const char* file = nullptr, int line = 0) noexcept;
        void Writef(LogLevel level, std::string_view category, const char* file, int line,
                    const char* format, ...) noexcept;
    }

    // Optional thread-safe handoff to a main-thread GUI. Has no GUI dependency.
    class LogBuffer
    {
    public:
        explicit LogBuffer(size_t capacity = 500);
        void Push(const LogRecord& record);
        std::vector<LogRecord> Drain();
        void Clear();
        uint64_t GetDroppedCount() const;
        static void Callback(const LogRecord& record, void* buffer);
    private:
        const size_t m_capacity;
        mutable std::mutex m_mutex;
        std::vector<LogRecord> m_records;
        uint64_t m_dropped = 0;
    };
}
