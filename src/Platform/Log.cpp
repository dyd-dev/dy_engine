#include "dyf/Platform/Log.h"
#include "LogSession.h"
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <utility>

namespace dyf::Platform
{
    namespace
    {
        struct State
        {
            std::atomic<LogLevel> level{LogLevel::Info};
            std::mutex mutex;
            Log::Callback callback = nullptr;
            void* userData = nullptr;
            bool output = true;
            uint64_t sequence = 0;
        };

        State& GetState()
        {
            // Process lifetime: logging remains valid during static object destruction.
            static State* state = new State;
            return *state;
        }

        bool Enabled(LogLevel level)
        {
            return level >= LogLevel::Debug && level < LogLevel::Off
                && level >= GetState().level.load(std::memory_order_relaxed);
        }

        std::string Escape(std::string_view value)
        {
            std::string result;
            for (unsigned char c : value)
            {
                switch (c)
                {
                case '\n': result += "\\n"; break;
                case '\r': result += "\\r"; break;
                case '\t': result += "\\t"; break;
                case '\\': result += "\\\\"; break;
                default:
                    if (c < 32 || c == 127)
                    {
                        const char hex[] = "0123456789abcdef";
                        result += "\\x";
                        result += hex[c >> 4];
                        result += hex[c & 15];
                    }
                    else result += static_cast<char>(c);
                }
            }
            return result;
        }

        std::string Format(const LogRecord& record)
        {
            const char* levels[] = {"Debug", "Info", "Warning", "Error", "Fatal"};
            const auto time = std::chrono::system_clock::to_time_t(record.timestamp);
            std::tm utc{};
#ifdef _WIN32
            gmtime_s(&utc, &time);
#else
            gmtime_r(&time, &utc);
#endif
            std::ostringstream text;
            text << '[' << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ")
                 << "][seq=" << record.sequence << "][thread=" << record.thread
                 << "][" << levels[static_cast<int>(record.level)] << "]["
                 << Escape(record.category) << "] " << Escape(record.message);
            if (!record.file.empty()) text << " (" << Escape(record.file) << ':' << record.line << ')';
            text << '\n';
            return text.str();
        }
    }

    void Log::Initialize() noexcept { LogInternal::InitializeSession(); }
    std::string Log::GetSessionDirectory() { return LogInternal::SessionDirectory(); }

    void Log::SetLevel(LogLevel level) noexcept
    {
        try { GetState().level.store(level, std::memory_order_relaxed); } catch (...) {}
    }

    void Log::SetCallback(Callback callback, void* userData) noexcept
    {
        try {
            auto& state = GetState();
            std::lock_guard<std::mutex> lock(state.mutex);
            state.callback = callback;
            state.userData = userData;
        } catch (...) {}
    }

    void Log::SetDefaultOutputEnabled(bool enabled) noexcept
    {
        try {
            auto& state = GetState();
            std::lock_guard<std::mutex> lock(state.mutex);
            state.output = enabled;
        } catch (...) {}
    }

    void Log::Write(LogLevel level, std::string_view category, std::string_view message,
                    const char* file, int line) noexcept
    {
        try
        {
            Initialize();
            if (!Enabled(level)) return;
            auto& state = GetState();
            LogRecord record;
            record.level = level;
            record.category = category;
            record.message = message;
            if (file) record.file = file;
            record.line = line;
            record.thread = std::this_thread::get_id();
            Callback callback;
            void* userData;
            {
                std::lock_guard<std::mutex> lock(state.mutex);
                record.sequence = ++state.sequence;
                record.timestamp = std::chrono::system_clock::now();
                callback = state.callback;
                userData = state.userData;
                if (state.output || LogInternal::HasSessionOutput())
                {
                    const auto text = Format(record);
                    if (state.output) {
                        std::fwrite(text.data(), 1, text.size(), stderr);
                        std::fflush(stderr);
                    }
                    LogInternal::WriteSession(text);
                }
            }
            static thread_local bool inCallback = false;
            if (callback && !inCallback)
            {
                inCallback = true;
                try { callback(record, userData); } catch (...) {}
                inCallback = false;
            }
        }
        catch (...) {} // Diagnostics must not turn an existing failure into an exception.
    }

    void Log::Writef(LogLevel level, std::string_view category, const char* file, int line,
                     const char* format, ...) noexcept
    {
        va_list args;
        va_start(args, format);
        try
        {
            if (format && Enabled(level))
            {
                va_list copy;
                va_copy(copy, args);
                const int size = std::vsnprintf(nullptr, 0, format, copy);
                va_end(copy);
                if (size >= 0)
                {
                    std::vector<char> message(static_cast<size_t>(size) + 1);
                    std::vsnprintf(message.data(), message.size(), format, args);
                    Write(level, category, std::string_view(message.data(), static_cast<size_t>(size)), file, line);
                }
            }
        }
        catch (...) {}
        va_end(args);
    }

    LogBuffer::LogBuffer(size_t capacity) : m_capacity(capacity) {}
    void LogBuffer::Callback(const LogRecord& record, void* buffer)
    {
        if (buffer) static_cast<LogBuffer*>(buffer)->Push(record);
    }
    void LogBuffer::Push(const LogRecord& record)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_capacity == 0) { ++m_dropped; return; }
        // Callbacks from different threads may arrive out of sequence.
        const auto pos = std::lower_bound(m_records.begin(), m_records.end(), record.sequence,
            [](const LogRecord& item, uint64_t sequence) { return item.sequence < sequence; });
        m_records.insert(pos, record);
        if (m_records.size() > m_capacity) { m_records.erase(m_records.begin()); ++m_dropped; }
    }
    std::vector<LogRecord> LogBuffer::Drain()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<LogRecord> records;
        records.swap(m_records);
        return records;
    }
    void LogBuffer::Clear()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_records.clear();
        m_dropped = 0;
    }
    uint64_t LogBuffer::GetDroppedCount() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_dropped;
    }
}
