#pragma once
#include <string>
#include <string_view>

namespace dyf::Platform::LogInternal
{
    void InitializeSession() noexcept;
    void WriteSession(std::string_view text) noexcept;
    bool HasSessionOutput() noexcept;
    std::string SessionDirectory();
}
