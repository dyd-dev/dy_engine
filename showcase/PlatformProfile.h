#pragma once

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <thread>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace showcase
{
// The same scene and features run on each host; only explicit resource budgets differ.
struct PlatformProfile
{
    bool mobile = false;

    static PlatformProfile Automatic() noexcept
    {
#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
        return {true};
#else
        return {false};
#endif
    }

    static PlatformProfile Parse(std::string_view value)
    {
        if(value == "auto") return Automatic();
        if(value == "desktop") return {false};
        if(value == "mobile") return {true};
        throw std::runtime_error("--profile requires auto, desktop, or mobile.");
    }

    const char* Name() const noexcept { return mobile ? "mobile" : "desktop"; }
    uint32_t Width() const noexcept { return mobile ? 1280u : 1920u; }
    uint32_t Height() const noexcept { return mobile ? 720u : 1080u; }
    uint32_t TextureLimit() const noexcept { return mobile ? 512u : 0u; }
    uint32_t EnvironmentLimit() const noexcept { return mobile ? 1024u : 0u; }
    uint32_t ProbeFaceSize() const noexcept { return mobile ? 128u : 256u; }
    uint32_t Workers() const noexcept
    {
        const uint32_t available = (std::max)(1u, std::thread::hardware_concurrency());
        return (std::min)(mobile ? 2u : 4u, available);
    }
};
}
