#pragma once
#include <atomic>
#include <filesystem>
#include <functional>
#include <string_view>

namespace viewer
{
struct ToolPaths
{
    std::filesystem::path tracy;
    std::filesystem::path renderdocCommand;
    std::filesystem::path renderdocUi;
};

using ToolBootstrapProgress = std::function<void(std::string_view)>;

// Prepares RenderDoc only; embedded Tracy needs no executable or cache download.
// Runs on the caller's worker thread, including progress callbacks. Throws on failure or cancellation.
ToolPaths EnsureProfilerTools(const std::filesystem::path& cacheRoot,
    const ToolBootstrapProgress& progress, const std::atomic_bool& cancel);
}
