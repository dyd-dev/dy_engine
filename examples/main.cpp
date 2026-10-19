#include "ExampleHost.h"
#include "ExampleCatalog.h"
#include "SceneAssets.h"
#include "dyf/Platform/Window.h"
#include "ExampleDeclarations.inc"
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <iterator>
#include <string_view>
#include <utility>

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif
#ifdef _WIN32
#include <Windows.h>
#elif defined(__APPLE__) && !TARGET_OS_IPHONE
#include <mach-o/dyld.h>
#endif

namespace
{
namespace fs = std::filesystem;
using dy_examples::Example;
const Example examples[] = {
#include "ExampleTargets.inc"
};

void ListExamples()
{
    for(std::size_t i = 0; i < std::size(examples); ++i)
    {
        const auto& example = examples[i];
        std::cout << i + 1 << ". " << example.name << " [" << example.category << "]\n";
        if(const auto* metadata = example_catalog::Find(example.name))
            std::cout << "   " << example_catalog::StatusLabel(metadata->status) << " / " << metadata->title
                << "\n   " << metadata->summary << "\n   Requires: " << metadata->requirements << '\n';
    }
}

void ListScenes()
{
    for(const auto& asset : dy_examples::sceneAssets)
        std::cout << asset.id << " / " << asset.name << " [" << asset.format << "]\n"
            << "   " << dy_examples::AssetAvailabilityName(asset.availability) << "\n   " << asset.workload
            << "\n   " << asset.license << "\n   " << asset.platformNotes << '\n';
}

const Example* Find(std::string_view selection)
{
    std::size_t number = 0;
    const auto parsed = std::from_chars(selection.data(), selection.data() + selection.size(), number);
    if(parsed.ec == std::errc{} && parsed.ptr == selection.data() + selection.size() &&
        number > 0 && number <= std::size(examples)) return &examples[number - 1];
    for(const auto& example : examples)
        if(selection == example.name) return &example;
    return nullptr;
}

void Help()
{
    std::cout << "visualizer [--asset-root DIR] [--scene ID|PATH]\n"
        "visualizer --list | --scenes\n"
        "visualizer number|target-name [example arguments...]\n"
        "Without a target name, open the scene workbench. Examples run in the same\n"
        "window; use the example controls to switch examples or return to the workbench.\n"
        "Arguments after a target name pass through unchanged. --list and --scenes need no GPU.\n";
}

#if !defined(__ANDROID__) && !(defined(__APPLE__) && TARGET_OS_IPHONE)
fs::path ExecutableDirectory(const char* argument)
{
#ifdef _WIN32
    std::vector<wchar_t> buffer(512);
    for(;;)
    {
        const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if(!length) break;
        if(length < buffer.size()) return fs::path(std::wstring(buffer.data(), length)).parent_path();
        buffer.resize(buffer.size() * 2);
    }
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(size);
    if(size && _NSGetExecutablePath(buffer.data(), &size) == 0)
        return fs::weakly_canonical(fs::path(buffer.data())).parent_path();
#else
    std::error_code error;
    const auto executable = fs::read_symlink("/proc/self/exe", error);
    if(!error) return executable.parent_path();
#endif
    return fs::absolute(argument && *argument ? fs::u8path(argument) : fs::path("visualizer")).parent_path();
}

#endif
}

int DyfRunExamples(int argc, char** argv, const char* assetRoot, const char* fontPath,
    void (*onExampleChanged)(bool)) try
{
    if(fontPath && *fontPath)
    {
#ifdef _WIN32
        _putenv_s("DY_EXAMPLES_FONT", fontPath);
#else
        setenv("DY_EXAMPLES_FONT", fontPath, 1);
#endif
    }
#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
    const fs::path repositoryRoot = fs::absolute(assetRoot && *assetRoot ? fs::u8path(assetRoot) : fs::path("."));
    fs::path assets = repositoryRoot;
#else
    const fs::path repositoryRoot = fs::weakly_canonical(
        ExecutableDirectory(argc > 0 ? argv[0] : nullptr) / DY_EXAMPLES_REPOSITORY_RELATIVE);
    fs::path assets = assetRoot && *assetRoot ? fs::absolute(fs::u8path(assetRoot)) : repositoryRoot.parent_path() / "assets";
#endif
    fs::path initialScene;
    const Example* explicitExample = nullptr;
    std::vector<std::string> arguments;
    bool list = false, scenes = false;
    for(int i = 1; i < argc; ++i)
    {
        const std::string_view argument(argv[i]);
        if(argument == "--help" || argument == "-h") { Help(); return 0; }
        if(argument == "--list") { list = true; continue; }
        if(argument == "--scenes") { scenes = true; continue; }
        if(argument == "--asset-root" || argument == "--scene")
        {
            if(++i == argc) { std::cerr << argument << " requires a value.\n"; return 2; }
            if(argument == "--asset-root") assets = fs::absolute(fs::u8path(argv[i]));
            else initialScene = fs::u8path(argv[i]);
            continue;
        }
        explicitExample = Find(argument);
        if(!explicitExample) { std::cerr << "Unknown example or option: " << argument << ". Use --help.\n"; return 2; }
        arguments.assign(argv + i + 1, argv + argc);
        break; // All following arguments belong to the selected example.
    }
    if(list || scenes)
    {
        if(list) ListExamples();
        if(scenes) ListScenes();
        return 0;
    }
    if(explicitExample && !initialScene.empty())
    {
        std::cerr << "--scene opens a workbench scene; use it without an example target.\n";
        return 2;
    }
    if(!initialScene.empty())
    {
        bool catalogId = false;
        for(const auto& asset : dy_examples::sceneAssets) catalogId |= initialScene.u8string() == asset.id;
        if(!catalogId) initialScene = fs::absolute(initialScene);
    }
    if(onExampleChanged) onExampleChanged(false);
    dyf::Platform::Window window(1280, 800, "dy_engine / Examples workbench");
    if(!window.GetHandle()) return 1;
    // The state is released before the shared native window and survives every
    // module visit. Each module scopes its rendering resources to that visit.
    std::shared_ptr<dy_examples::WorkbenchState> workbench;
    const Example* selected = explicitExample;
    std::string status;
    while(window.IsRunning())
    {
        if(!selected)
        {
            arguments.clear();
            const int result = dy_examples::OpenWorkbench(window, workbench, examples,
                std::size(examples), selected, arguments, status, assets, repositoryRoot, initialScene);
            initialScene.clear();
            if(result || !selected) return result;
        }
        auto result = dy_examples::RunHostedExample(window, examples, std::size(examples),
            *selected, arguments, repositoryRoot);
        status = !result.message.empty() ? std::move(result.message) : std::string(selected->name) +
            (result.code == 0 ? " closed." : result.code == 77 ? ": unavailable on this backend." :
                ": exited with code " + std::to_string(result.code) + ".");
        selected = result.next;
        arguments = std::move(result.arguments);
    }
    return 0;
}
catch(const std::exception& error)
{
    std::cerr << error.what() << '\n';
    return 1;
}

#if !defined(__ANDROID__) && !(defined(__APPLE__) && TARGET_OS_IPHONE)
int main(int argc, char** argv)
{
    return DyfRunExamples(argc, argv, nullptr, nullptr, nullptr);
}
#endif
